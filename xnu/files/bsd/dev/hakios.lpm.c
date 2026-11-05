/*
 * DT-described low-power levels for Munch.
 *
 * CPU rail-PC and cluster LLCC-off use the DT-described PSCI states. The
 * broadcast timer is handed to the always-on PDC before reset-based suspend.
 */
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/hakios.qca6390.h>
#include <sys/systm.h>

extern void loader_log_putc(char c);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t psci_smp_cpu_suspend(uint32_t state);
extern uint64_t psci_smp_next_idle_deadline(uint64_t local_timeout);
extern bool hakios_ddr_lpm_ready(void);

#define HAKIOS_LPM_CPU_GROUPS 2
#define HAKIOS_LPM_RSC_PHYS 0x18220000UL
#define HAKIOS_LPM_RSC_SIZE 0x10000UL
#define HAKIOS_LPM_RSC_PDC_DATA 0x38
#define HAKIOS_LPM_RSC_TCS(t, reg) (0xd00 + 0x2a0 * (t) + (reg))
#define HAKIOS_LPM_RSC_ACTIVE_TCS_COUNT 2
#define HAKIOS_LPM_PDC_VALID (1u << 31)
#define HAKIOS_LPM_PDC_HI_MASK 0x00ffffffu
#define HAKIOS_LPM_NO_DEADLINE UINT64_MAX

struct hakios_lpm_level {
	uint32_t state;
	uint32_t entry_us;
	uint32_t exit_us;
	uint32_t min_residency_us;
	uint32_t min_child_idx;
	bool valid;
	bool is_reset;
	bool use_broadcast_timer;
	bool notify_rpm;
};

static struct hakios_lpm_level cpu_levels[HAKIOS_LPM_CPU_GROUPS];
static struct hakios_lpm_level cluster_level;
static vm_offset_t lpm_rsc_base;
static uint32_t lpm_cpu_timer_add_us[HAKIOS_LPM_CPU_GROUPS];
static uint32_t lpm_cluster_timer_add_us;
static uint32_t lpm_cluster_mode_shift;
static uint32_t lpm_cluster_mode_mask;
static uint32_t lpm_online_mask;
static uint32_t lpm_idle_mask;
static bool lpm_initialized;
static bool lpm_enabled;
static bool lpm_cluster_enabled;
static LCK_GRP_DECLARE(lpm_lock_group, "hakios-lpm");
static LCK_SPIN_DECLARE(lpm_lock, &lpm_lock_group);

static void
lpm_tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

static uint32_t
lpm_u32(DTEntry entry, const char *name, uint32_t fallback)
{
	const void *value;
	unsigned int size;
	uint32_t result;

	if (SecureDTGetProperty(entry, name, &value, &size) != kSuccess || size != sizeof(result)) {
		return fallback;
	}
	bcopy(value, &result, sizeof(result));
	return result;
}

static bool
lpm_flag(DTEntry entry, const char *name)
{
	const void *value;
	unsigned int size;

	return SecureDTGetProperty(entry, name, &value, &size) == kSuccess && size == 0;
}

static bool
lpm_level(DTEntry parent, const char *wanted_label, struct hakios_lpm_level *level)
{
	OpaqueDTEntryIterator iterator;
	DTEntry child;
	const void *value;
	unsigned int size;

	if (SecureDTInitEntryIterator(parent, &iterator) != kSuccess) {
		return false;
	}
	while (SecureDTIterateEntries(&iterator, &child) == kSuccess) {
		if (SecureDTGetProperty(child, "label", &value, &size) != kSuccess ||
		    size != strlen(wanted_label) + 1 || memcmp(value, wanted_label, size - 1) != 0) {
			continue;
		}
		level->state = lpm_u32(child, "qcom,psci-cpu-mode", lpm_u32(child, "qcom,psci-mode", 0));
		level->entry_us = lpm_u32(child, "qcom,entry-latency-us", 0);
		level->exit_us = lpm_u32(child, "qcom,exit-latency-us", 0);
		level->min_residency_us = lpm_u32(child, "qcom,min-residency-us", 0);
		level->min_child_idx = lpm_u32(child, "qcom,min-child-idx", 0);
		level->is_reset = lpm_flag(child, "qcom,is-reset");
		level->use_broadcast_timer = lpm_flag(child, "qcom,use-broadcast-timer");
		level->notify_rpm = lpm_flag(child, "qcom,notify-rpm");
		level->valid = level->state != 0 && level->min_residency_us != 0;
		return level->valid;
	}
	return false;
}

static bool
lpm_rsc_idle(void)
{
	if (lpm_rsc_base == 0) {
		return false;
	}
	for (uint32_t tcs = 0; tcs < HAKIOS_LPM_RSC_ACTIVE_TCS_COUNT; tcs++) {
		if (*(volatile uint32_t *)(lpm_rsc_base + HAKIOS_LPM_RSC_TCS(tcs, 0x18)) == 0) {
			return false;
		}
	}
	return true;
}

static uint64_t
lpm_deadline_ticks(uint64_t deadline)
{
	uint64_t now = mach_absolute_time(), elapsed_ns, seconds, remainder;
	uint64_t counter, frequency, delta;

	if (deadline == HAKIOS_LPM_NO_DEADLINE || deadline <= now) {
		return HAKIOS_LPM_NO_DEADLINE;
	}
	absolutetime_to_nanoseconds(deadline - now, &elapsed_ns);
	counter = __builtin_arm_rsr64("CNTVCT_EL0");
	frequency = __builtin_arm_rsr64("CNTFRQ_EL0");
	seconds = elapsed_ns / NSEC_PER_SEC;
	remainder = elapsed_ns % NSEC_PER_SEC;
	if (frequency == 0 || seconds > (UINT64_MAX - counter) / frequency) {
		return HAKIOS_LPM_NO_DEADLINE;
	}
	delta = seconds * frequency + remainder * frequency / NSEC_PER_SEC;
	return delta > UINT64_MAX - counter ? HAKIOS_LPM_NO_DEADLINE : counter + delta;
}

static bool
lpm_program_wakeup(uint64_t deadline)
{
	uint64_t ticks = lpm_deadline_ticks(deadline);
	uint32_t high = (uint32_t)(ticks >> 32) & HAKIOS_LPM_PDC_HI_MASK;
	uint32_t low = (uint32_t)ticks;

	if (ticks == HAKIOS_LPM_NO_DEADLINE) {
		high = HAKIOS_LPM_PDC_VALID | HAKIOS_LPM_PDC_HI_MASK;
		low = UINT32_MAX;
	} else {
		high |= HAKIOS_LPM_PDC_VALID;
	}
	lck_spin_lock(&lpm_lock);
	if (!lpm_rsc_idle()) {
		lck_spin_unlock(&lpm_lock);
		return false;
	}
	*(volatile uint32_t *)(lpm_rsc_base + HAKIOS_LPM_RSC_PDC_DATA) = high;
	*(volatile uint32_t *)(lpm_rsc_base + HAKIOS_LPM_RSC_PDC_DATA + 8) = low;
	__asm__ volatile ("dsb sy" ::: "memory");
	lck_spin_unlock(&lpm_lock);
	return true;
}

static uint64_t
lpm_required_ns(const struct hakios_lpm_level *level, uint32_t timer_add_us)
{
	return ((uint64_t)level->entry_us + level->exit_us + level->min_residency_us +
	    timer_add_us) * NSEC_PER_USEC;
}

void
hakios_lpm_cpu_wake(void)
{
	uint32_t cpu = (uint32_t)cpu_number();

	if (!lpm_initialized || cpu >= 32) return;
	lck_spin_lock(&lpm_lock);
	lpm_idle_mask &= ~(1u << cpu);
	lck_spin_unlock(&lpm_lock);
}

void
hakios_lpm_init(void)
{
	DTEntry cpu_group, cluster, lpm, system_pm;
	uint32_t enabled = 0;
	uint32_t cluster_enabled = 1;
	const ml_topology_info_t *topology;

	if (!hakios_qca6390_platform_matches()) {
		return;
	}
	PE_parse_boot_argn("lpm", &enabled, sizeof(enabled));
	lpm_enabled = enabled != 0;
	if (PE_parse_boot_argn("lpm_llcc", &cluster_enabled, sizeof(cluster_enabled))) {
		lpm_cluster_enabled = cluster_enabled != 0;
	} else {
		lpm_cluster_enabled = lpm_enabled;
	}
	if (SecureDTFindEntry("compatible", "qcom,lpm-levels", &lpm) != kSuccess ||
	    SecureDTLookupEntry(lpm, "qcom,pm-cluster@0", &cluster) != kSuccess ||
	    SecureDTFindEntry("compatible", "qcom,system-pm", &system_pm) != kSuccess) {
		lpm_tell("lpm: qcom,lpm-levels is unavailable\n");
		return;
	}
	lpm_level(cluster, "llcc-off", &cluster_level);
	lpm_cluster_timer_add_us = lpm_u32(cluster, "qcom,clstr-tmr-add", 0);
	lpm_cluster_mode_shift = lpm_u32(cluster, "qcom,psci-mode-shift", 0);
	lpm_cluster_mode_mask = lpm_u32(cluster, "qcom,psci-mode-mask", 0);
	if (SecureDTLookupEntry(cluster, "qcom,pm-cpu@0", &cpu_group) == kSuccess) {
		lpm_level(cpu_group, "rail-pc", &cpu_levels[0]);
		lpm_cpu_timer_add_us[0] = lpm_u32(cpu_group, "qcom,tmr-add", 0);
	}
	if (SecureDTLookupEntry(cluster, "qcom,pm-cpu@1", &cpu_group) == kSuccess) {
		lpm_level(cpu_group, "rail-pc", &cpu_levels[1]);
		lpm_cpu_timer_add_us[1] = lpm_u32(cpu_group, "qcom,tmr-add", 0);
	}
	lpm_rsc_base = ml_io_map(HAKIOS_LPM_RSC_PHYS, HAKIOS_LPM_RSC_SIZE);
	topology = ml_get_topology_info();
	lpm_online_mask = topology->num_cpus >= 32 ? UINT32_MAX :
	    topology->num_cpus == 0 ? 0 : (1u << topology->num_cpus) - 1;
	lpm_initialized = lpm_rsc_base != 0 && lpm_online_mask != 0 &&
	    (cpu_levels[0].valid || cpu_levels[1].valid);
	lpm_cluster_enabled = lpm_cluster_enabled && hakios_ddr_lpm_ready() &&
	    cluster_level.valid && cluster_level.notify_rpm &&
	    lpm_cluster_mode_shift != 0 && lpm_cluster_mode_mask != 0;
	lpm_tell("lpm: rail-pc little=%u/%u/%u us big=%u/%u/%u us, llcc-off=%s, lpm=%u cluster=%u\n",
	    cpu_levels[0].entry_us, cpu_levels[0].exit_us, cpu_levels[0].min_residency_us,
	    cpu_levels[1].entry_us, cpu_levels[1].exit_us, cpu_levels[1].min_residency_us,
	    lpm_cluster_enabled ? "RPMh/PDC-ready" : cluster_level.valid ? "gated" : "absent", lpm_enabled ? 1 : 0,
	    lpm_cluster_enabled ? 1 : 0);
}

bool
hakios_lpm_idle(uint64_t idle_timeout_ticks)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t core, group, cpu, state;
	struct hakios_lpm_level *level;
	uint64_t now, deadline, idle_ns, required_ns, cluster_required_ns;
	bool cluster_ready;

	if (!lpm_initialized || !lpm_enabled) {
		return false;
	}
	cpu = (uint32_t)cpu_number();
	if (cpu >= topology->num_cpus || cpu >= 32) return false;
	core = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	group = core < 4 ? 0 : 1;
	level = &cpu_levels[group];
	if (!level->valid || !level->is_reset || !level->use_broadcast_timer || lpm_rsc_base == 0) {
		return false;
	}
	now = mach_absolute_time();
	deadline = psci_smp_next_idle_deadline(idle_timeout_ticks);
	if (deadline == HAKIOS_LPM_NO_DEADLINE) {
		idle_ns = UINT64_MAX;
	} else if (deadline <= now) {
		return false;
	} else {
		absolutetime_to_nanoseconds(deadline - now, &idle_ns);
	}
	required_ns = lpm_required_ns(level, lpm_cpu_timer_add_us[group]);
	if (idle_ns < required_ns || !lpm_program_wakeup(deadline)) {
		return false;
	}
	lck_spin_lock(&lpm_lock);
	lpm_idle_mask |= 1u << cpu;
	cluster_ready = lpm_cluster_enabled && cluster_level.min_child_idx <= 1 &&
	    (lpm_idle_mask & lpm_online_mask) == lpm_online_mask;
	lck_spin_unlock(&lpm_lock);

	state = (1u << 30) | level->state;
	if (cluster_ready) {
		cluster_required_ns = lpm_required_ns(&cluster_level, lpm_cluster_timer_add_us);
		if (idle_ns >= cluster_required_ns) {
			state |= (cluster_level.state & lpm_cluster_mode_mask) << lpm_cluster_mode_shift;
			state |= 1u << 24;
		}
	}
	if (psci_smp_cpu_suspend(state) != KERN_SUCCESS) {
		hakios_lpm_cpu_wake();
		return false;
	}
	hakios_lpm_cpu_wake();
	return true;
}

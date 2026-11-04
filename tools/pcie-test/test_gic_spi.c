#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <mach/kern_return.h>

typedef bool boolean_t;
#define FALSE false
#define LCK_GRP_NULL NULL
#define GIC_SPI_MOST 48
#define GIC_PRIORITY_DEFAULT 0xa0
#define decl_simple_lock_data(storage, name) pthread_mutex_t name = PTHREAD_MUTEX_INITIALIZER
static void simple_lock(pthread_mutex_t *lock, void *group) { assert(group == NULL); assert(pthread_mutex_lock(lock) == 0); }
static void simple_unlock(pthread_mutex_t *lock) { assert(pthread_mutex_unlock(lock) == 0); }
static _Thread_local bool interrupts_enabled = true, interrupt_context;
static boolean_t ml_set_interrupts_enabled(boolean_t enable)
{ bool previous = interrupts_enabled; interrupts_enabled = enable; return previous; }
static bool ml_at_interrupt_context(void) { return interrupt_context; }
static uintptr_t gicd_base = 0x10000000;
static size_t gicd_size = 0x10000;
static uint8_t registers[0x10000], initial[0x10000];
static unsigned writes, fail_write, rwp_after, rwp_reads;
static bool stuck_rwp;
static uint64_t affinity = 0x200;

static uint32_t get32(size_t offset) { uint32_t value; memcpy(&value, registers + offset, sizeof(value)); return value; }
static void put32(size_t offset, uint32_t value) { memcpy(registers + offset, &value, sizeof(value)); }
static uint32_t gic_read32(uintptr_t address)
{
	size_t offset = address - gicd_base;
	assert(offset + 4 <= sizeof(registers));
	if (offset == 0) {
		rwp_reads++;
		if (stuck_rwp || (rwp_after != 0 && writes >= rwp_after)) { return get32(offset) | (1u << 31); }
	}
	return get32(offset);
}
static uint8_t gic_read8(uintptr_t address) { assert(address - gicd_base < sizeof(registers)); return registers[address - gicd_base]; }
static uint64_t gic_read64(uintptr_t address)
{ uint64_t value; assert(address - gicd_base + 8 <= sizeof(registers)); memcpy(&value, registers + (address - gicd_base), sizeof(value)); return value; }
static bool ignore_write(void) { writes++; return writes == fail_write; }
static void gic_write32(uintptr_t address, uint32_t value)
{
	size_t offset = address - gicd_base;
	assert(offset + 4 <= sizeof(registers));
	if (ignore_write()) { return; }
	if (offset >= 0x100 && offset < 0x180) { put32(offset, get32(offset) | value); }
	else if (offset >= 0x180 && offset < 0x200) { put32(offset - 0x80, get32(offset - 0x80) & ~value); }
	else if (offset >= 0x280 && offset < 0x300) { put32(offset - 0x80, get32(offset - 0x80) & ~value); }
	else { put32(offset, value); }
}
static void gic_write8(uintptr_t address, uint8_t value)
{ assert(address - gicd_base < sizeof(registers)); if (!ignore_write()) { registers[address - gicd_base] = value; } }
static void gic_write64(uintptr_t address, uint64_t value)
{ assert(address - gicd_base + 8 <= sizeof(registers)); if (!ignore_write()) { memcpy(registers + (address - gicd_base), &value, sizeof(value)); } }
static void gic_spi_barrier(void) { atomic_thread_fence(memory_order_seq_cst); }
static uint64_t gic_spi_affinity(void) { return affinity; }

#include "gic_v3_spi.inc"

static unsigned arrived;
static atomic_bool handler_entered, handler_continue;
static void handler(void *context, uint32_t intid)
{ assert(context == &arrived && intid >= 800 && intid < 832); arrived++; }
static void blocked_handler(void *context, uint32_t intid)
{
	handler(context, intid);
	atomic_store(&handler_entered, true);
	while (!atomic_load(&handler_continue)) { sched_yield(); }
}
static void reset(void)
{
	for (unsigned index = 0; index < GIC_SPI_MOST; index++) { assert(gic_spi[index].lease == 0); }
	memset(registers, 0, sizeof(registers));
	put32(SPI_CTLR, 0x12);
	put32(SPI_TYPER, 31);
	for (unsigned number = 32; number < 1020; number++) {
		put32(SPI_GROUP(number), UINT32_MAX);
		registers[SPI_PRIORITY(number)] = 0x80;
		put32(SPI_CONFIG(number), 0x55555555);
		uint64_t router = number;
		memcpy(registers + SPI_ROUTER(number), &router, sizeof(router));
	}
	memcpy(initial, registers, sizeof(initial));
	writes = fail_write = rwp_after = rwp_reads = arrived = 0;
	stuck_rwp = false;
	interrupts_enabled = true; interrupt_context = false;
}
static void check_restored(void)
{ assert(memcmp(registers, initial, sizeof(initial)) == 0 && interrupts_enabled); }
static void attach_bank(gic_v3_spi_lease_t lease, unsigned count)
{
	for (unsigned vector = 0; vector < count; vector++) {
		assert(gic_v3_spi_bank_attach(lease, vector, true, handler, &arrived) == KERN_SUCCESS);
		uint32_t intid;
		assert(gic_v3_spi_bank_intid(lease, vector, &intid) == KERN_SUCCESS && intid == 800 + vector);
		assert((get32(SPI_ENABLED(intid)) & (1u << (intid % 32))) == 0);
	}
}
static void test_lifecycle(void)
{
	reset();
	gic_v3_spi_lease_t lease, other;
	assert(gic_v3_spi_bank_reserve(800, 32, &lease) == KERN_SUCCESS && writes == 0);
	assert(gic_v3_spi_bank_reserve(810, 1, &other) == KERN_NAME_EXISTS && other == 0);
	assert(gic_v3_spi_attach(800, true, handler, &arrived) == KERN_NAME_EXISTS);
	assert(gic_v3_spi_bank_enable(lease, 0, true) == KERN_FAILURE && writes == 0);
	uint32_t intid = 12;
	assert(gic_v3_spi_bank_intid(lease, 0, &intid) == KERN_FAILURE && intid == 0);
	attach_bank(lease, 32);
	for (unsigned vector = 0; vector < 32; vector++) {
		assert(gic_v3_spi_bank_enable(lease, vector, true) == KERN_SUCCESS);
		unsigned before = writes;
		gic_v3_spi_mask(800 + vector); gic_v3_spi_unmask(800 + vector); gic_v3_spi_detach(800 + vector);
		assert(writes == before);
		struct gic_spi *slot = gic_spi_dispatch(800 + vector);
		assert(slot != NULL && slot->busy == 1 && gic_v3_spi_count(800 + vector) == 1);
		gic_spi_complete(slot);
	}
	assert(arrived == 32);
	gic_v3_spi_lease_t stale = lease;
	assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS && lease == 0);
	check_restored();
	assert(gic_v3_spi_bank_reserve(800, 32, &lease) == KERN_SUCCESS && lease != stale);
	assert(gic_v3_spi_bank_enable(stale, 0, true) == KERN_INVALID_ARGUMENT);
	assert(gic_v3_spi_bank_release(&stale) == KERN_INVALID_ARGUMENT);
	assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
	assert(gic_v3_spi_attach(800, true, handler, &arrived) == KERN_SUCCESS);
	assert(get32(SPI_ENABLED(800)) == 1);
	gic_v3_spi_mask(800); assert(get32(SPI_ENABLED(800)) == 0);
	gic_v3_spi_unmask(800); assert(get32(SPI_ENABLED(800)) == 1);
	gic_v3_spi_detach(800);
	check_restored();
}
static void test_failures(void)
{
	for (unsigned failure = 1; failure <= 3; failure++) {
		reset();
		gic_v3_spi_lease_t lease;
		assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
		fail_write = failure;
		assert(gic_v3_spi_bank_attach(lease, 0, true, handler, &arrived) == KERN_FAILURE);
		assert(lease != 0 && gic_v3_spi_bank_enable(lease, 0, true) == KERN_FAILURE);
		fail_write = 0;
		assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
		check_restored();
	}
	for (unsigned scenario = 0; scenario < 4; scenario++) {
		reset();
		gic_v3_spi_lease_t lease;
		assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
		if (scenario == 0) { stuck_rwp = true; }
		if (scenario == 1) { rwp_after = 1; }
		if (scenario <= 1) {
			assert(gic_v3_spi_bank_attach(lease, 0, true, handler, &arrived) == KERN_OPERATION_TIMED_OUT);
			assert(rwp_reads >= SPI_WAIT_BUDGET);
		} else {
			attach_bank(lease, 1);
			assert(gic_v3_spi_bank_enable(lease, 0, true) == KERN_SUCCESS);
			if (scenario == 2) { fail_write = writes + 1; }
			else { stuck_rwp = true; }
			assert(gic_v3_spi_bank_release(&lease) != KERN_SUCCESS && lease != 0);
			gic_v3_spi_lease_t other;
			assert(gic_v3_spi_bank_reserve(800, 1, &other) == KERN_NAME_EXISTS);
		}
		stuck_rwp = false; rwp_after = fail_write = 0;
		assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
		check_restored();
	}
	for (unsigned failure = 1; failure <= 5; failure++) {
		reset();
		gic_v3_spi_lease_t lease;
		assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
		attach_bank(lease, 1);
		assert(gic_v3_spi_bank_enable(lease, 0, true) == KERN_SUCCESS);
		put32(SPI_PENDING(800), 1);
		fail_write = writes + failure;
		assert(gic_v3_spi_bank_release(&lease) == KERN_FAILURE && lease != 0);
		fail_write = 0;
		assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
		check_restored();
	}
}
static void test_preconditions(void)
{
	for (unsigned scenario = 0; scenario < 5; scenario++) {
		reset();
		gic_v3_spi_lease_t lease;
		assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
		if (scenario == 0) { put32(SPI_GROUP(800), 0); }
		if (scenario == 1) { put32(SPI_ENABLED(800), 1); }
		if (scenario == 2) { put32(SPI_ACTIVE(800), 1); }
		if (scenario == 3) { put32(SPI_PENDING(800), 1); }
		if (scenario == 4) { put32(SPI_CTLR, 0); }
		assert(gic_v3_spi_bank_attach(lease, 0, true, handler, &arrived) == KERN_PROTECTION_FAILURE && writes == 0);
		assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS && writes == 0);
	}
	reset();
	gic_v3_spi_lease_t lease, second, third;
	assert(gic_v3_spi_bank_reserve(31, 1, &lease) == KERN_INVALID_ARGUMENT);
	assert(gic_v3_spi_bank_reserve(1000, 32, &lease) == KERN_INVALID_ARGUMENT);
	assert(gic_v3_spi_bank_reserve(800, 0, &lease) == KERN_INVALID_ARGUMENT);
	put32(SPI_TYPER, 23);
	assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_INVALID_ARGUMENT);
	put32(SPI_TYPER, 31);
	assert(gic_v3_spi_bank_reserve(800, 32, &lease) == KERN_SUCCESS);
	assert(gic_v3_spi_bank_reserve(832, 16, &second) == KERN_SUCCESS);
	assert(gic_v3_spi_bank_reserve(848, 1, &third) == KERN_RESOURCE_SHORTAGE);
	assert(gic_v3_spi_bank_release(&second) == KERN_SUCCESS);
	assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
	check_restored();
}
static void *dispatch_thread(void *unused)
{
	(void)unused;
	interrupt_context = true; interrupts_enabled = false;
	struct gic_spi *slot = gic_spi_dispatch(800);
	assert(slot != NULL);
	gic_spi_complete(slot);
	assert(!interrupts_enabled);
	return NULL;
}
static void test_completion_lifetime(void)
{
	reset();
	gic_v3_spi_lease_t lease;
	assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
	assert(gic_v3_spi_bank_attach(lease, 0, true, blocked_handler, &arrived) == KERN_SUCCESS);
	assert(gic_v3_spi_bank_enable(lease, 0, true) == KERN_SUCCESS);
	atomic_store(&handler_entered, false); atomic_store(&handler_continue, false);
	pthread_t worker;
	assert(pthread_create(&worker, NULL, dispatch_thread, NULL) == 0);
	while (!atomic_load(&handler_entered)) { sched_yield(); }
	assert(gic_v3_spi_bank_release(&lease) == KERN_OPERATION_TIMED_OUT && lease != 0);
	gic_v3_spi_lease_t other;
	assert(gic_v3_spi_bank_reserve(800, 1, &other) == KERN_NAME_EXISTS);
	atomic_store(&handler_continue, true);
	assert(pthread_join(worker, NULL) == 0);
	assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
	check_restored();
	reset();
	assert(gic_v3_spi_bank_reserve(800, 1, &lease) == KERN_SUCCESS);
	attach_bank(lease, 1);
	assert(gic_v3_spi_bank_enable(lease, 0, true) == KERN_SUCCESS);
	struct gic_spi *slot = gic_spi_dispatch(800);
	assert(slot != NULL);
	assert(gic_v3_spi_bank_quiesce(lease) == KERN_OPERATION_TIMED_OUT);
	gic_spi_complete(slot);
	put32(SPI_ACTIVE(800), 1);
	assert(gic_v3_spi_bank_quiesce(lease) == KERN_OPERATION_TIMED_OUT);
	put32(SPI_ACTIVE(800), 0);
	assert(gic_v3_spi_bank_release(&lease) == KERN_SUCCESS);
	check_restored();
}
int main(void)
{
	test_lifecycle(); test_failures(); test_preconditions(); test_completion_lifetime();
	puts("gic-spi-test: PASS (actual controller bank core, masked attach, shared legacy exclusion, bounded RWP, fault retention, post-handler/EOI lifetime)");
	return 0;
}

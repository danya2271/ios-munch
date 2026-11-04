#ifndef PCIE_TEST_ATH11K_H
#define PCIE_TEST_ATH11K_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <sys/hakios.qmi.h>

#define GFP_KERNEL 0
#define ATH11K_BUS_PCI 0
#define ATH11K_HW_QCA6390_HW20 1
#define ATH11K_FIRMWARE_MODE_NORMAL 0
#define ATH11K_FLAG_UNREGISTERING 0
#define ATH11K_FLAG_QMI_FAIL 1
#define ATH11K_FLAG_MULTI_MSI_VECTORS 2
#define ATH11K_FLAG_DEVICE_INIT_DONE 3
#define CE_COUNT_MAX 12
#define CE_ATTR_DIS_INTR 1
#define ATH11K_EXT_IRQ_GRP_NUM_MAX 11
#define PIPEDIR_IN 1
#define PIPEDIR_OUT 2
#define PIPEDIR_INOUT 3
#define __le32_to_cpu(value) (value)

struct device { unsigned cookie; };
struct work_struct { unsigned tag; };
struct timer_list { unsigned tag; };
struct ath11k_base;
struct service_to_pipe { uint32_t service_id, pipedir, pipenum; };
struct ath11k_hw_ring_mask {
	uint8_t tx[11], rx[11], rx_err[11], rx_wbm_rel[11], reo_status[11], rxdma2host[11], host2rxdma[11], rx_mon_status[11];
};
struct ath11k_ext_irq_grp { struct ath11k_base *ab; uint32_t grp_id; };
struct ath11k_hif_ops {
	uint32_t (*read32)(struct ath11k_base *, uint32_t);
	void (*write32)(struct ath11k_base *, uint32_t, uint32_t);
	int (*read)(struct ath11k_base *, void *, uint32_t, uint32_t);
	void (*irq_enable)(struct ath11k_base *);
	void (*irq_disable)(struct ath11k_base *);
	int (*start)(struct ath11k_base *);
	void (*stop)(struct ath11k_base *);
	int (*power_up)(struct ath11k_base *);
	void (*power_down)(struct ath11k_base *, bool);
	int (*suspend)(struct ath11k_base *);
	int (*resume)(struct ath11k_base *);
	int (*map_service_to_pipe)(struct ath11k_base *, uint16_t, uint8_t *, uint8_t *);
	int (*get_user_msi_vector)(struct ath11k_base *, char *, int *, uint32_t *, uint32_t *);
	void (*get_msi_address)(struct ath11k_base *, uint32_t *, uint32_t *);
	void (*ce_irq_enable)(struct ath11k_base *);
	void (*ce_irq_disable)(struct ath11k_base *);
	void (*get_ce_msi_idx)(struct ath11k_base *, uint32_t, uint32_t *);
};

struct ath11k_msi_user { char *name; int num_vectors; uint32_t base_vector; };
struct ath11k_msi_config {
	int total_vectors;
	int total_users;
	struct ath11k_msi_user *users;
	int hw_rev;
};

struct ath11k_base {
	struct device *dev;
	struct { const struct ath11k_hif_ops *ops; int bus; } hif;
	int hw_rev;
	int fw_mode;
	unsigned long dev_flags;
	struct { uint16_t vendor, device, subsystem_vendor, subsystem_device; } id;
	struct { struct {
		const struct ath11k_msi_config *config;
		uint32_t addr_lo, addr_hi, ep_base_data;
	} msi; } pci;
	struct { void *srng_config; } hal;
	struct {
		uint32_t target_ce_count, svc_to_ce_map_len, qmi_service_ins_id;
		const void *target_ce_config;
		const struct service_to_pipe *svc_to_ce_map;
		uint32_t ce_count;
		const struct ath11k_hw_ring_mask *ring_mask;
	} hw_params;
	struct {
		struct qmi_handle handle;
		struct work_struct event_work;
		struct { uint32_t tgt_ce_len, svc_to_ce_map_len; const void *tgt_ce, *svc_to_ce_map; } ce_cfg;
		uint32_t service_ins_id;
	} qmi;
	struct work_struct reset_work, dump_work, restart_work, update_11d_work;
	struct timer_list rx_replenish_retry;
	struct ath11k_ext_irq_grp ext_irq_grp[ATH11K_EXT_IRQ_GRP_NUM_MAX];
	_Alignas(void *) unsigned char drv_priv[sizeof(void *)];
};

static inline void set_bit(unsigned bit, unsigned long *value) { *value |= 1ul << bit; }
static inline bool test_bit(unsigned bit, const unsigned long *value) { return (*value & (1ul << bit)) != 0; }
void *kzalloc(size_t size, unsigned flags);
void kfree(void *memory);
bool cancel_work_sync(struct work_struct *work);
int del_timer_sync(struct timer_list *timer);
struct ath11k_base *ath11k_core_alloc(struct device *, size_t, int);
int ath11k_core_pre_init(struct ath11k_base *);
int ath11k_hal_srng_init(struct ath11k_base *);
void ath11k_hal_srng_deinit(struct ath11k_base *);
int ath11k_ce_alloc_pipes(struct ath11k_base *);
void ath11k_ce_free_pipes(struct ath11k_base *);
int ath11k_core_init(struct ath11k_base *);
void ath11k_core_deinit(struct ath11k_base *);
void ath11k_core_free(struct ath11k_base *);
void ath11k_fw_destroy(struct ath11k_base *);
void ath11k_debugfs_soc_destroy(struct ath11k_base *);
void ath11k_qmi_deinit_service(struct ath11k_base *);
void ath11k_core_pm_notifier_unregister(struct ath11k_base *);
uint32_t ath11k_ce_get_attr_flags(struct ath11k_base *, uint32_t);
void ath11k_ce_rx_post_buf(struct ath11k_base *);
void ath11k_ce_cleanup_pipes(struct ath11k_base *);
void ath11k_ce_per_engine_service(struct ath11k_base *, uint16_t);
int ath11k_dp_service_srng(struct ath11k_base *, struct ath11k_ext_irq_grp *, int);

#endif

/*
 * QMI, the protocol in which the driver talks to the radio's firmware before the radio's own
 * registers exist. The helpers are the kernel's (bsd/sys/hakios.qmi.h, bsd/dev/hakios.qmi.c),
 * under Linux's names; a kernel tree that does not have them yet gets LinuxKPI's header, in
 * which every call fails.
 */
#ifndef _LX_LINUX_SOC_QCOM_QMI_H
#define _LX_LINUX_SOC_QCOM_QMI_H

#ifdef LX_HAKIOS_QMI_H
#include <linux/jiffies.h>
#define HAKIOS_QMI_JIFFIES_TO_MS(j)	jiffies_to_msecs(j)
#include LX_HAKIOS_QMI_H
#if defined(CONFIG_ATH11K_PCI)
#if defined(CONFIG_ATH11K_AHB)
#error "Select a single native ath11k bus profile"
#endif
int hakios_ipcr_qmi_handle_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *operations,
    const struct qmi_msg_handler *handlers);
#define qmi_handle_init hakios_ipcr_qmi_handle_init
#endif
/*
 * What the driver sends goes past the radio's bus first (src/lx_bus.c), which has to know of one
 * request: the mode the firmware is asked for. The radio's registers answer in mission mode only.
 */
int lx_qmi_send_request(struct qmi_handle *qmi, struct sockaddr_qrtr *sq, struct qmi_txn *txn, int msg_id, size_t largest,
    const struct qmi_elem_info *ei, const void *structure);
#if !defined(LX_QMI_AS_IT_IS) && !defined(CONFIG_ATH11K_PCI)
#define qmi_send_request	lx_qmi_send_request
#endif
#else
#include <linux/socket.h>
#include_next <linux/soc/qcom/qmi.h>
#endif

#endif

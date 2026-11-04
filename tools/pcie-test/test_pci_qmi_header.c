#define CONFIG_ATH11K_PCI 1
#define LX_HAKIOS_QMI_H <sys/hakios.qmi.h>
#include "../../wifi/lx/include/linux/soc/qcom/qmi.h"

int
test_pci_qmi_init(struct qmi_handle *qmi, size_t largest, const struct qmi_ops *operations)
{
	return qmi_handle_init(qmi, largest, operations, NULL);
}

int
test_pci_qmi_send(struct qmi_handle *qmi, struct qmi_txn *transaction,
    const struct qmi_elem_info *elements, const void *data)
{
	return qmi_send_request(qmi, NULL, transaction, 0x55, 0, elements, data);
}

/* SPDX-License-Identifier: APSL-2.0
 * USB descriptors and protocol sequence adapted from the UMI loader at
 * 6b6fb2b0c5a1720496ec71e88c95d2a45a0046a4. See README.md for provenance.
 * The probe accepts only information queries, ping, and reboot.
 */
#include "usb-commands.h"

static const uint8_t probe_device[18] = {18,1,0x10,2,0,0,0,64,0xd1,0x18,0x0d,0xd0,0,1,1,2,3,1};
static const uint8_t probe_ss_device[18] = {18,1,0,3,0,0,0,9,0xd1,0x18,0x0d,0xd0,0,1,1,2,3,1};
static const uint8_t probe_config[32] = {
    9,2,32,0,1,1,0,0x80,0x50, 9,4,0,0,2,0xff,0x42,3,4,
    7,5,0x81,2,0,2,0, 7,5,1,2,0,2,0
};
static const uint8_t probe_ss_config[44] = {
    9,2,44,0,1,1,0,0x80,0x10, 9,4,0,0,2,0xff,0x42,3,4,
    7,5,0x81,2,0,4,0, 6,0x30,4,0,0,0,
    7,5,1,2,0,4,0, 6,0x30,4,0,0,0
};
static const uint8_t probe_qualifier[10] = {10,6,0,2,0xff,0xff,0xff,64,1,0};
static const uint8_t probe_bos[] = {
    5,0x0f,42,0,3, 7,0x10,2,6,0,0,0,
    10,0x10,3,0,0x0e,0,1,7,0x65,0,
    20,0x10,0x0a,0,1,0,0,0,0,0x11,0,0,0x30,0x40,0x0a,0,0xb0,0x40,0x0a,0
};
_Static_assert(sizeof(probe_bos) == 42, "BOS descriptor length mismatch");
static const uint8_t probe_language[4] = {4,3,9,4};
static const uint8_t probe_manufacturer[8] = {8,3,'X',0,'N',0,'U',0};
static const uint8_t probe_product[22] = {22,3,'m',0,'u',0,'n',0,'c',0,'h',0,'p',0,'r',0,'o',0,'b',0,'e',0};
static const uint8_t probe_serial[22] = {22,3,'M',0,'U',0,'N',0,'C',0,'H',0,'P',0,'R',0,'O',0,'B',0,'E',0};
static const uint8_t probe_interface[18] = {18,3,'f',0,'a',0,'s',0,'t',0,'b',0,'o',0,'o',0,'t',0};

static void EFIAPI probe_notify(void *event, void *context)
{
    (void)event; (void)context;
}

static uint64_t probe_ticks(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}

static void usb_probe(efi_system_table *table)
{
    const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
    const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
    usb_device_protocol *usb = NULL;
    void *event = NULL;
    efi_status status = table->bs->create_event_ex(0x200, 8, probe_notify, NULL, &init_guid, &event);
    field("USB init event status: ", status);
    if (!EFI_ERROR(status)) {
        field("USB init signal status: ", table->bs->signal_event(event));
        table->bs->close_event(event);
    }
    status = table->bs->locate_protocol(&usb_guid, NULL, (void **)&usb);
    if (EFI_ERROR(status) || !usb) { field("USB lookup failed: ", status); return; }
    const void *config = probe_config, *ss_config = probe_ss_config;
    const void *strings[] = {probe_language, probe_manufacturer, probe_product, probe_serial, probe_interface};
    usb_device_descriptor_set descriptors = {
        probe_device, &config, probe_ss_device, &ss_config,
        probe_qualifier, probe_bos, 5, strings
    };
    status = usb->start_ex(&descriptors);
    field("USB StartEx status: ", status);
    if (EFI_ERROR(status)) return;
    uint8_t *rx = NULL, *tx = NULL;
    status = usb->allocate_transfer_buffer(1024, (void **)&rx);
    if (EFI_ERROR(status)) { field("USB RX buffer error: ", status); return; }
    status = usb->allocate_transfer_buffer(1024, (void **)&tx);
    if (EFI_ERROR(status)) { field("USB TX buffer error: ", status); return; }
    text("USB probe active for 300 seconds.\r\nHost: fastboot getvar product; fastboot oem ping\r\n");
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    uint64_t started = probe_ticks();
    int connected = 0, rx_pending = 0, tx_pending = 0, reboot_pending = 0;
    unsigned commands = 0;
    while (probe_ticks() - started < frequency * 300) {
        if (connected && !rx_pending && !tx_pending) {
            status = usb->send(1, 511, rx);
            if (EFI_ERROR(status)) { field("USB receive queue error: ", status); break; }
            rx_pending = 1;
        }
        uint32_t message = UsbDeviceEventNoEvent;
        uint64_t size = 0;
        usb_device_event_data data;
        status = usb->handle_event(&message, &size, &data);
        if (EFI_ERROR(status)) { field("USB event error: ", status); break; }
        if (message == UsbDeviceEventDeviceStateChange) {
            connected = data.device_state == UsbDeviceStateConnected;
            rx_pending = tx_pending = reboot_pending = 0;
            text(connected ? "USB connected\r\n" : "USB disconnected\r\n");
        } else if (message == UsbDeviceEventTransferNotification) {
            if (data.transfer.endpoint_index == 0x81) {
                tx_pending = 0;
                if (data.transfer.status != UsbDeviceTransferStatusCompleteOK) {
                    field("USB reply transfer status: ", data.transfer.status);
                    reboot_pending = 0;
                } else if (reboot_pending) {
                    text("Host requested reboot.\r\n");
                    return;
                }
            } else if (data.transfer.endpoint_index == 1) {
                rx_pending = 0;
                if (data.transfer.status != UsbDeviceTransferStatusCompleteOK ||
                    data.transfer.bytes_completed == 0 || data.transfer.bytes_completed > 511)
                    continue;
                rx[data.transfer.bytes_completed] = 0;
                const char *reply = probe_command((const char *)rx);
                reboot_pending = probe_equal((const char *)rx, "reboot");
                unsigned length = 0;
                while (reply[length]) { tx[length] = (uint8_t)reply[length]; length++; }
                status = usb->send(0x81, length, tx);
                if (EFI_ERROR(status)) { field("USB reply queue error: ", status); break; }
                tx_pending = 1;
                if (++commands <= 16) field("USB commands received: ", commands);
            }
        }
        table->bs->stall(100);
    }
    text("USB probe session ended.\r\n");
}

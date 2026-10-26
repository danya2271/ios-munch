/* SPDX-License-Identifier: MIT
 * F4 UEFI bring-up probe. Optional read-only storage probe; no kernel handoff.
 */
#include "efi.h"

static efi_system_table *system_table;
static uint64_t map_buffer[8192];

static void text(const char *value)
{
    char16 buffer[2] = {0, 0};
    while (*value) {
        buffer[0] = (unsigned char)*value++;
        system_table->con_out->output_string(system_table->con_out, buffer);
    }
}

static void hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char16 buffer[19];
    buffer[0] = '0'; buffer[1] = 'x'; buffer[18] = 0;
    for (unsigned i = 0; i < 16; i++)
        buffer[2 + i] = digits[(value >> (60 - 4 * i)) & 15];
    system_table->con_out->output_string(system_table->con_out, buffer);
}

static void field(const char *name, uint64_t value)
{
    text(name); hex(value); text("\r\n");
}

#ifdef MUNCH_GPU_PROBE
#include "gpu-probe.inc"
#endif

#ifdef MUNCH_USB_PROBE
#include "usb-probe.inc"
#endif

#ifdef MUNCH_TOUCH_POWER_PROBE
#include "touch-power-probe.inc"
#endif

#ifdef MUNCH_TOUCH_POWER_ENABLE
#include "touch-power-enable.inc"
#endif

#ifdef MUNCH_TOUCH_PROBE
#include "touch-probe.inc"
#endif

#ifdef MUNCH_ROOT_PROBE
#include "root-probe.inc"
#endif

efi_status efi_main(efi_handle image, efi_system_table *table)
{
    (void)image;
    system_table = table;
    if (!table || !table->bs || !table->con_out || !table->rt)
        return EFI_ERROR_BIT | 2;
    table->bs->set_watchdog_timer(0, 0, 0, NULL);
    text("\r\nMUNCH UEFI DIAGNOSTIC 1\r\nNo XNU or iOS is running.\r\n");
    const efi_guid gop_guid = EFI_GOP_GUID;
    const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
    efi_gop *gop = NULL;
    void *usb = NULL;
    efi_status status = table->bs->locate_protocol(&gop_guid, NULL, (void **)&gop);
    field("GOP status: ", status);
    if (!EFI_ERROR(status) && gop && gop->mode && gop->mode->info) {
        field("Width: ", gop->mode->info->width);
        field("Height: ", gop->mode->info->height);
        field("Stride pixels: ", gop->mode->info->pixels_per_scanline);
        field("Pixel format: ", gop->mode->info->pixel_format);
        field("Framebuffer: ", gop->mode->fb_base);
    }
    status = table->bs->locate_protocol(&usb_guid, NULL, &usb);
    field("Qualcomm USB protocol status: ", status);
    text("Protocol presence does not verify USB transfers.\r\n");

    uint64_t size = sizeof(map_buffer), key = 0, stride = 0;
    uint32_t version = 0;
    status = table->bs->get_memory_map(&size, map_buffer, &key, &stride, &version);
    field("Memory map status: ", status);
    if (!EFI_ERROR(status) && stride >= sizeof(efi_memory_descriptor) && size <= sizeof(map_buffer)) {
        unsigned shown = 0, bad = 0;
        for (uint64_t offset = 0; offset + stride <= size; offset += stride) {
            const efi_memory_descriptor *desc = (const void *)((const uint8_t *)map_buffer + offset);
            if (desc->type != EfiConventionalMemory)
                continue;
            uint64_t start = desc->phys_start, end = start + desc->pages * 4096;
            if ((start < 0xC0000000ULL && end > 0xBCC00000ULL) ||
                (start < 0xF4000000ULL && end > 0xF0000000ULL))
                bad++;
            if (desc->pages >= 0x4000 && shown++ < 8) {
                text("Free >=64MiB: "); hex(start); text(" .. "); hex(end); text("\r\n");
            }
        }
        field("Gap/payload reservation violations: ", bad);
    }
#ifdef MUNCH_TOUCH_POWER_ENABLE
    touch_power_probe();
    if (touch_power_enable_begin()) touch_probe(table, gop);
    touch_power_enable_end();
    touch_power_probe();
#else
#ifdef MUNCH_TOUCH_POWER_PROBE
    touch_power_probe();
#endif
#ifdef MUNCH_TOUCH_PROBE
    touch_probe(table, gop);
#endif
#endif
#ifdef MUNCH_GPU_PROBE
    gpu_probe();
#endif
#ifdef MUNCH_ROOT_PROBE
    root_probe(table);
#endif
#ifdef MUNCH_USB_PROBE
    usb_probe(table);
#else
    text("Photograph this screen. Reboot attempt in 90 seconds.\r\n");
    for (unsigned i = 0; i < 90; i++)
        table->bs->stall(1000000);
#endif
    table->rt->reset_system(0, 0, 0, NULL);
    return 0;
}

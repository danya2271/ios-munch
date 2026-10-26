/* SPDX-License-Identifier: MIT
 * Read-only iosroot discovery. No write/flush protocol methods are called.
 */
static uint8_t root_probe_buffer[65536] __attribute__((aligned(8192)));

static void root_probe(efi_system_table *table)
{
    const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
    const efi_guid block_guid = EFI_BLOCK_IO_GUID;
    static const char16 wanted[] = u"iosroot";
    efi_handle *handles = NULL;
    uint64_t count = 0, matches = 0;
    efi_block_io *selected = NULL;
    text("\r\nMUNCH IOSROOT READ-ONLY PROBE 2\r\n");
    efi_handle *blocks = NULL;
    uint64_t block_count = 0;
    efi_status block_status = table->bs->locate_handle_buffer(ByProtocol, &block_guid,
                                                            NULL, &block_count, &blocks);
    field("Block I/O enumeration status: ", block_status);
    field("Block I/O handles: ", block_count);
    if (!EFI_ERROR(block_status)) {
        typedef efi_status (EFIAPI *connect_fn)(efi_handle, efi_handle *,
                                               efi_device_path *, uint8_t);
        connect_fn connect = (connect_fn)table->bs->connect_controller;
        for (uint64_t i = 0; i < block_count; i++) {
            efi_status connected = connect(blocks[i], NULL, NULL, 1);
            field("Block controller connect status: ", connected);
        }
        table->bs->free_pool(blocks);
    }
    efi_status status = table->bs->locate_handle_buffer(ByProtocol, &part_guid,
                                                       NULL, &count, &handles);
    field("Partition enumeration status: ", status);
    if (EFI_ERROR(status)) return;
    for (uint64_t i = 0; i < count; i++) {
        efi_partition_info *part = NULL;
        efi_block_io *bio = NULL;
        if (EFI_ERROR(table->bs->handle_protocol(handles[i], &part_guid, (void **)&part)) ||
            !part || part->type != 2) continue;
        unsigned n = 0;
        while (n < sizeof(wanted) / sizeof(wanted[0]) && part->name[n] == wanted[n]) n++;
        if (n != sizeof(wanted) / sizeof(wanted[0])) continue;
        matches++;
        if (!EFI_ERROR(table->bs->handle_protocol(handles[i], &block_guid, (void **)&bio)))
            selected = bio;
    }
    table->bs->free_pool(handles);
    field("iosroot matches: ", matches);
    if (matches != 1 || !selected || !selected->media) {
        text("IOSROOT READ: NOT ATTEMPTED\r\n"); return;
    }
    const efi_block_media *media = selected->media;
    field("Block size: ", media->block_size);
    field("Last block: ", media->last_block);
    if (!media->block_size || media->block_size > sizeof(root_probe_buffer) ||
        sizeof(root_probe_buffer) % media->block_size ||
        media->last_block < sizeof(root_probe_buffer) / media->block_size - 1 ||
        media->io_align > 8192 ||
        (media->io_align > 1 && (uint64_t)(uintptr_t)root_probe_buffer % media->io_align)) {
        text("IOSROOT READ: UNSUPPORTED GEOMETRY\r\n"); return;
    }
    status = selected->read_blocks(selected, media->media_id, 0,
                                   sizeof(root_probe_buffer), root_probe_buffer);
    field("iosroot ReadBlocks status: ", status);
    if (EFI_ERROR(status)) { text("IOSROOT READ: FAILED\r\n"); return; }
    text("IOSROOT READ: PASS (64 KiB)\r\n");
    text(root_probe_buffer[32] == 'N' && root_probe_buffer[33] == 'X' &&
         root_probe_buffer[34] == 'S' && root_probe_buffer[35] == 'B'
         ? "APFS container magic: NXSB\r\n" : "APFS container magic: absent at offset 32\r\n");
    text("No partition was modified.\r\n");
}

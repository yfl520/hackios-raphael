/* Read-only offline boot resources. The installed data write policy is unchanged. */
#include "resource-config.h"
struct recovery_bundle_header {
	uint8_t magic[8];
	uint32_t version, header_bytes;
	uint64_t total_bytes, payload_offset, payload_bytes, resident_offset, resident_bytes;
	uint32_t payload_crc, resident_crc, header_crc, flags;
};
_Static_assert(sizeof(struct recovery_bundle_header) == 72, "bundle layout");

static uint32_t recovery_crc_table[256];
static void recovery_crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (unsigned k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		recovery_crc_table[i] = c;
	}
}
static uint32_t recovery_crc_update(uint32_t c, const uint8_t *p, uint64_t n)
{
	while (n--) c = recovery_crc_table[(c ^ *p++) & 255] ^ (c >> 8);
	return c;
}

/* Bounce through the existing aligned buffer; firmware never DMAs into XNU's window. */
static int recovery_read(efi_block_io *bio, uint64_t offset, uint64_t bytes,
    uint8_t *dest, uint32_t expected_crc, const char *label)
{
	uint64_t done = 0;
	uint32_t crc = 0xffffffffu;
	const uint64_t bs = 4096, capacity = bio->media->last_block + 1;
	if (offset % bs || !bytes || offset / bs >= capacity ||
	    (bytes + bs - 1) / bs > capacity - offset / bs) return 0;
	while (done < bytes) {
		uint64_t n = bytes - done > (1u << 20) ? (1u << 20) : bytes - done;
		uint64_t rounded = (n + bs - 1) & ~(bs - 1);
		efi_status s = bio->read_blocks(bio, bio->media->media_id,
		    (offset + done) / bs, rounded, block_buffer);
		if (EFI_ERROR(s)) { print("recovery: %s read failed at %x: %x\n", label, offset + done, s); return 0; }
		crc = recovery_crc_update(crc, block_buffer, n);
		memcpy(dest + done, block_buffer, n);
		done += n;
		wdt_pet();
	}
	crc = ~crc;
	print("recovery: %s %d bytes, CRC %x\n", label, bytes, (uint64_t)crc);
	return crc == expected_crc;
}

static efi_block_io *recovery_find_darwin(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID, blk_guid = EFI_BLOCK_IO_GUID;
	static const efi_guid type = BLOCK_DARWIN_TYPE;
	efi_handle *handles = NULL;
	efi_block_io *found = NULL;
	uint64_t count = 0, matches = 0;
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) return NULL;
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[36];
		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) continue;
		ufs_name(info, name, sizeof(name));
		if (!str_eq(name, "darwin")) continue;
		matches++;
		if (memcmp(&info->type_guid, &type, sizeof(type)) ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || !bio->media || !bio->read_blocks) continue;
		if (!bio->media->logical || !bio->media->present || bio->media->block_size != 4096 ||
		    bio->media->io_align > 8192 || bio->media->last_block < RECOVERY_DARWIN_MIN - 1 ||
		    bio->media->last_block > 0x0fffffffULL ||
		    info->end_lba < info->start_lba ||
		    info->end_lba - info->start_lba != bio->media->last_block) continue;
		found = bio;
	}
	BS->free_pool(handles);
	return matches == 1 ? found : NULL;
}

/* A recovery consumes boot-recovery once; otherwise every later cold reset re-enters it.
 * Only the exact empty-argument request is consumed. Preserve the other 4064 bytes.
 * This does not expose misc writes through XNU's block service. */
static int recovery_consume_request(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID, blk_guid = EFI_BLOCK_IO_GUID;
	static const efi_guid misc_type = { 0x82acc91f, 0x357c, 0x4a68, { 0x9c, 0x8f, 0x68, 0x9e, 0x1b, 0x1a, 0x23, 0xa1 } };
	static const uint8_t request[32] = "boot-recovery";
	static const uint8_t empty[32];
	static uint8_t expected[4096] __attribute__((aligned(8192)));
	efi_handle *handles = NULL;
	efi_block_io *found = NULL;
	uint64_t count = 0, matches = 0;
	if (PLAT != &platform_raphael ||
	    EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) return 0;
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[36];
		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) continue;
		ufs_name(info, name, sizeof(name));
		if (!str_eq(name, "misc")) continue;
		matches++;
		if (memcmp(&info->type_guid, &misc_type, sizeof(misc_type)) ||
		    info->start_lba != 1024 || info->end_lba != 2047 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || !bio->media ||
		    !bio->media->logical || !bio->media->present || bio->media->block_size != 4096 ||
		    bio->media->last_block != 1023 || bio->media->io_align > 8192 ||
		    !bio->read_blocks || !bio->write_blocks || !bio->flush_blocks || bio->media->read_only) continue;
		found = bio;
	}
	BS->free_pool(handles);
	if (matches != 1 || !found) { print("recovery: compatible misc not found; request left intact\n"); return 0; }
	uint32_t id = found->media->media_id;
	efi_status s = found->read_blocks(found, id, 0, 4096, block_buffer);
	if (EFI_ERROR(s)) { print("recovery: cannot read boot request %x\n", s); return 0; }
	if (!memcmp(block_buffer, empty, sizeof(empty))) {
		print("recovery: boot request already empty; misc not written\n"); return 1;
	}
	if (memcmp(block_buffer, request, sizeof(request))) {
		print("recovery: unfamiliar boot request; misc left intact\n"); return 0;
	}
	for (unsigned i = 64; i < 832; i++) {
		if (block_buffer[i]) { print("recovery: Android recovery arguments present; request left intact\n"); return 0; }
	}
	memcpy(expected, block_buffer, sizeof(expected));
	memset(expected, 0, sizeof(request));
	s = found->write_blocks(found, id, 0, sizeof(expected), expected);
	if (!EFI_ERROR(s)) s = found->flush_blocks(found);
	if (!EFI_ERROR(s)) s = found->read_blocks(found, id, 0, 4096, block_buffer);
	if (EFI_ERROR(s) || memcmp(expected, block_buffer, sizeof(expected))) {
		print("recovery: boot request clear/readback failed %x; no kernel started\n", s); return 0;
	}
	print("recovery: consumed boot-recovery once; all other misc bytes preserved, readback verified\n");
	return 1;
}

static int recovery_load(void)
{
	struct recovery_bundle_header h;
	struct payload_info pi;
	efi_block_io *bio;
	uint64_t started = READ_SYSREG(CNTVCT_EL0);
	if (PLAT != &platform_raphael || !(bio = recovery_find_darwin())) {
		print("recovery: unique compatible darwin partition not found\n"); return 0;
	}
	efi_status s = bio->read_blocks(bio, bio->media->media_id, RECOVERY_RESOURCE_OFFSET / 4096, 4096, block_buffer);
	if (EFI_ERROR(s)) { print("recovery: header read failed %x\n", s); return 0; }
	memcpy(&h, block_buffer, sizeof(h));
	((struct recovery_bundle_header *)block_buffer)->header_crc = 0;
	if (memcmp(h.magic, "RAPHIOS1", 8) || h.version != 2 || h.header_bytes != 4096 || h.flags != 1 ||
	    h.total_bytes != RECOVERY_BUNDLE_BYTES || h.payload_offset != 4096 ||
	    h.payload_bytes != RECOVERY_PAYLOAD_BYTES || h.resident_offset != RECOVERY_RESIDENT_OFFSET ||
	    h.resident_bytes != RECOVERY_RESIDENT_BYTES ||
	    ufs_crc32(block_buffer, 4096) != h.header_crc ||
	    RECOVERY_RESOURCE_OFFSET + h.total_bytes > BLOCK_WRITE_FIRST * 4096ULL ||
	    h.payload_bytes > PAYLOAD_MAX_SIZE - 0x100000ULL) {
		print("recovery: resource header invalid; no kernel started\n"); return 0;
	}
	recovery_crc_init();
	if (!recovery_read(bio, RECOVERY_RESOURCE_OFFSET + h.payload_offset, h.payload_bytes,
	    (uint8_t *)PAYLOAD_BASE, h.payload_crc, "payload")) goto fail;
	if (!parse_payload(&pi, 0) || !pi.kernel || !pi.ramdisk || pi.forced_base || pi.resident_rd || pi.checkpoint ||
	    !choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE ||
	    h.resident_bytes > win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE) goto fail;
	if (!recovery_read(bio, RECOVERY_RESOURCE_OFFSET + h.resident_offset, h.resident_bytes,
	    (uint8_t *)(win_base + RESIDENT_RD_OFFSET), h.resident_crc, "resident")) goto fail;
	if (!recovery_consume_request()) goto fail;
	resident_rd_size = h.resident_bytes;
	/* Replace only the ten epoch digits after validating the immutable payload. */
	uint64_t epoch = firmware_epoch();
	if (epoch >= 1000000000ULL && epoch <= 9999999999ULL &&
	    str_len(pi.cmdline) >= 16 && memcmp(pi.cmdline, "epoch=", 6) == 0 && pi.cmdline[16] == ' ') {
		char *digits = (char *)pi.cmdline + 6;
		for (int i = 9; i >= 0; i--) { digits[i] = '0' + epoch % 10; epoch /= 10; }
		print("recovery: boot time refreshed from firmware RTC\n");
	}
	print("recovery: resources verified, ready in %d ms; no host upload\n",
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / READ_SYSREG(CNTFRQ_EL0));
	return 1;
fail:
	resident_rd_size = 0;
	*(uint32_t *)PAYLOAD_BASE = 0;
	print("recovery: resource load failed; no kernel started\n");
	return 0;
}

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include "resource-config.h"
#define BLOCK_WRITE_FIRST 2042368ULL
#define PAYLOAD_MAX_SIZE 0x04000000ULL
typedef uint64_t efi_status;
#define EFI_ERROR(s) ((s)!=0)
static void print(const char *s,...){(void)s;}
static void wdt_pet(void){}
static uint8_t block_buffer[1048576];
typedef struct {uint64_t last_block;uint32_t media_id;} Media;
typedef struct IO IO;
struct IO {Media *media;efi_status (*read_blocks)(IO*,uint32_t,uint64_t,uint64_t,void*);};
typedef IO efi_block_io;
static uint8_t disk[8192];static unsigned io_calls;
static efi_status read_blocks(IO*b,uint32_t id,uint64_t lba,uint64_t bytes,void*out){
 (void)b;assert(id==7);++io_calls;if(lba+bytes/4096>2)return 1;memcpy(out,disk+lba*4096,bytes);return 0;
}
static uint32_t ufs_crc32(const void*raw,uint64_t n){
 uint32_t c=~0u;const uint8_t*p=raw;
 while(n--){c^=*p++;for(unsigned j=0;j<8;j++)c=(c>>1)^((0u-(c&1))&0xedb88320u);}return ~c;
}
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


static int valid(struct recovery_bundle_header h){
 memset(block_buffer,0,4096);memcpy(block_buffer,&h,sizeof(h));
 ((struct recovery_bundle_header*)block_buffer)->header_crc=0;
 return !(memcmp(h.magic, "RAPHIOS1", 8) || h.version != 2 || h.header_bytes != 4096 || h.flags != 1 ||
	    h.total_bytes != RECOVERY_BUNDLE_BYTES || h.payload_offset != 4096 ||
	    h.payload_bytes != RECOVERY_PAYLOAD_BYTES || h.resident_offset != RECOVERY_RESIDENT_OFFSET ||
	    h.resident_bytes != RECOVERY_RESIDENT_BYTES ||
	    ufs_crc32(block_buffer, 4096) != h.header_crc ||
	    RECOVERY_RESOURCE_OFFSET + h.total_bytes > BLOCK_WRITE_FIRST * 4096ULL ||
	    h.payload_bytes > PAYLOAD_MAX_SIZE - 0x100000ULL);
}
static void seal(struct recovery_bundle_header*h){memset(block_buffer,0,4096);memcpy(block_buffer,h,sizeof(*h));((struct recovery_bundle_header*)block_buffer)->header_crc=0;h->header_crc=ufs_crc32(block_buffer,4096);}
int main(){
 struct recovery_bundle_header h={.magic="RAPHIOS1",.version=2,.header_bytes=4096,.total_bytes=RECOVERY_BUNDLE_BYTES,.payload_offset=4096,.payload_bytes=RECOVERY_PAYLOAD_BYTES,.resident_offset=RECOVERY_RESIDENT_OFFSET,.resident_bytes=RECOVERY_RESIDENT_BYTES,.payload_crc=123,.resident_crc=456,.flags=1};
 seal(&h);assert(valid(h));
 h.payload_crc=11111;h.resident_crc=22222;seal(&h);assert(valid(h));
 struct recovery_bundle_header good=h;
 h.version=1;seal(&h);assert(!valid(h));h=good;
 h.flags=0;seal(&h);assert(!valid(h));h=good;
 h.payload_bytes++;seal(&h);assert(!valid(h));h=good;
 h.resident_offset-=4096;seal(&h);assert(!valid(h));h=good;
 h.resident_bytes++;seal(&h);assert(!valid(h));h=good;
 h.total_bytes++;seal(&h);assert(!valid(h));h=good;
 h.header_crc^=1;assert(!valid(h));
 recovery_crc_init();for(unsigned i=0;i<sizeof(disk);i++)disk[i]=(i*31+7)&255;
 Media media={.last_block=1,.media_id=7};IO bio={&media,read_blocks};uint8_t out[8192];
 uint32_t crc=ufs_crc32(disk,6160);
 assert(recovery_read(&bio,0,6160,out,crc,"test") && !memcmp(out,disk,6160));
 assert(!recovery_read(&bio,0,6160,out,crc^1,"test"));
 unsigned before=io_calls;assert(!recovery_read(&bio,1,4096,out,crc,"test"));assert(io_calls==before);
 assert(!recovery_read(&bio,4096,8192,out,crc,"test"));assert(io_calls==before);
 puts("PASS actual header predicate and resource reader: dynamic CRC, geometry/version/flag corruption, content corruption, alignment and read bounds");
}

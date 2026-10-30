/* Shared with xnuloader. Caller supplies fixed-width integer types. */
#ifndef HAKIOS_LOADER_DISPLAY_H
#define HAKIOS_LOADER_DISPLAY_H
#define LOADER_DISPLAY_MAGIC 0x31594c5053445752ULL
#define LOADER_DISPLAY_VERSION 1
struct loader_display {
	uint64_t magic;
	uint32_t version, size;
	uint8_t header_padding[48];
	struct {
		volatile uint32_t sequence;
		uint32_t percent;
		uint8_t padding[56];
	} request;
	struct {
		volatile uint32_t sequence;
		uint32_t percent;
		uint64_t status;
		uint8_t padding[48];
	} answer;
};
#endif

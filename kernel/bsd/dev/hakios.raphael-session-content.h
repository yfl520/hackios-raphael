/* Exact byte counts, eight bytes at a time with scalar integer operations.
 * No SIMD/FPU state in kernel context, no sampling of the RAW frame. */
#ifndef HAKIOS_RAPHAEL_CONTENT_H
#define HAKIOS_RAPHAEL_CONTENT_H
struct raphael_content_counts { unsigned zero, written_rows; };

static unsigned
raphael_zero_bytes(uint64_t word)
{
    const uint64_t low = 0x7f7f7f7f7f7f7f7full;
    /* The low-seven-bit additions cannot carry into adjacent bytes. Unlike
     * (x-0x0101...) & ~x, this mask has no false positives after a zero byte. */
    uint64_t high = ~(((word & low) + low) | word | low);
    return (unsigned)(((high >> 7) * 0x0101010101010101ull) >> 56);
}

static struct raphael_content_counts
raphael_content_count(const uint8_t *raw, unsigned stride, unsigned rows,
    unsigned row_bytes, unsigned minimum_written)
{
    struct raphael_content_counts result = {0};
    for (unsigned y = 0; y < rows; y++) {
        const uint8_t *line = raw + (size_t)y * stride;
        unsigned written = 0, x = 0;
        for (; x + 8 <= row_bytes; x += 8) {
            uint64_t word;
            __builtin_memcpy(&word, line + x, sizeof(word));
            result.zero += raphael_zero_bytes(word);
            written += 8 - raphael_zero_bytes(word ^ 0xa5a5a5a5a5a5a5a5ull);
        }
        for (; x < row_bytes; x++) {
            result.zero += line[x] == 0;
            written += line[x] != 0xa5;
        }
        result.written_rows += written > minimum_written;
    }
    return result;
}
#endif

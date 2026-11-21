/* Storage accounting for one persistent darwin partition and its HFS data area.
 * No on-disk I/O; reject incoherent geometry before exposing it to Settings.
 */
#ifndef RAPHAEL_STORAGE_ACCOUNTING_H
#define RAPHAEL_STORAGE_ACCOUNTING_H
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
struct raphael_storage_counts {
    uint64_t total, free, available, used, reserved, data_used;
};
static bool raphael_storage_account(uint64_t partition, uint64_t block_size,
    uint64_t blocks, uint64_t free_blocks, uint64_t available_blocks,
    struct raphael_storage_counts *out)
{
    if(!out || !partition || partition>INT64_MAX || !block_size || !blocks ||
        blocks>UINT64_MAX/block_size || free_blocks>blocks ||
        available_blocks>free_blocks)return false;
    uint64_t data=blocks*block_size;
    if(data>partition)return false;
    struct raphael_storage_counts value={
        .total=partition, .free=free_blocks*block_size,
        .available=available_blocks*block_size,
        .reserved=partition-data, .data_used=(blocks-free_blocks)*block_size,
    };
    /* Space unavailable to normal apps remains counted as occupied/reserved. */
    value.used=partition-value.available;
    *out=value;
    return true;
}
#endif

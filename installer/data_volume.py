"""Build an empty HFSX data volume for the checked GPT size, from the signed-off empty template.

Only initial installation calls this module. Existing mounted data is never resized.
The original journal, catalog, attributes and extent trees remain byte-identical.
The allocation bitmap and both volume headers describe the actual new capacity.
"""
import struct
from disk import BS, DATA_FIRST, sparse_chunks, need

MIN_BLOCKS = 5242875
MAX_BLOCKS = (1024 ** 4) // BS  # Far above Raphael storage; also bound bitmap memory.


def initial_extents(template, darwin_blocks):
    need(type(darwin_blocks) is int and MIN_BLOCKS <= darwin_blocks <= MAX_BLOCKS,
         'iOS 分区大小不在支持范围')
    blocks = {}
    for offset, data in sparse_chunks(template):
        need(offset % BS == 0 and offset >= DATA_FIRST * BS, '初始数据模板覆盖系统区域')
        for at in range(0, len(data), BS):
            key = (offset + at) // BS - DATA_FIRST
            need(key not in blocks, '初始数据模板有重复块')
            blocks[key] = data[at:at + BS]
    header = bytearray(blocks[0][1024:1536])
    need(header[:4] == b'HX\x00\x05', '初始数据模板不是 HFSX')
    size, original_total, original_free = struct.unpack_from('>III', header, 40)
    need(size == BS and len(blocks) == original_total - original_free,
         '初始数据模板的分配块统计不一致')
    need(blocks[original_total - 1][3072:3584] == header, '初始数据模板主副卷头不一致')
    need(struct.unpack_from('>I', header, 4)[0] & 0x2000, '初始数据模板缺少日志')
    logical, _, allocation_count = struct.unpack_from('>QII', header, 112)
    start, count = struct.unpack_from('>II', header, 128)
    need(count == allocation_count and logical == count * BS and
         header[136:192] == bytes(56), '初始分配文件布局不支持')
    old_bitmap = b''.join(blocks[start + i] for i in range(count))
    used = {i * 8 + bit for i, value in enumerate(old_bitmap) if value
            for bit in range(8) if value & (128 >> bit)}
    need(used == set(blocks), '初始数据模板与分配位图不一致')
    del blocks[original_total - 1]
    for block in range(start, start + count):
        del blocks[block]
    total = darwin_blocks - DATA_FIRST
    bitmap_count = (total + BS * 8 - 1) // (BS * 8)
    bitmap_start = ((max(blocks) + 1 + 511) // 512) * 512
    need(bitmap_start + bitmap_count < total - 1, '数据卷太小，无法容纳文件系统元数据')
    bitmap = bytearray(bitmap_count * BS)
    allocated = set(blocks) | set(range(bitmap_start, bitmap_start + bitmap_count)) | {total - 1}
    for block in allocated:
        need(0 <= block < total, '数据卷元数据越界')
        bitmap[block // 8] |= 128 >> (block % 8)
    struct.pack_into('>III', header, 40, BS, total, total - len(allocated))
    struct.pack_into('>I', header, 52, bitmap_start + bitmap_count)
    struct.pack_into('>QII', header, 112, len(bitmap), len(bitmap), bitmap_count)
    header[128:192] = struct.pack('>II', bitmap_start, bitmap_count) + bytes(56)
    primary = bytearray(blocks[0]); primary[1024:1536] = header; blocks[0] = bytes(primary)
    alternate = bytearray(BS); alternate[3072:3584] = header; blocks[total - 1] = bytes(alternate)
    for i in range(bitmap_count):
        blocks[bitmap_start + i] = bytes(bitmap[i * BS:(i + 1) * BS])
    need(set(blocks) == allocated, '数据卷最终分配块统计不一致')
    # Bound every write and coalesce adjacent initialized blocks.
    first = previous = None
    run = bytearray()
    for block in sorted(blocks):
        if previous is not None and block != previous + 1:
            yield (DATA_FIRST + first) * BS, bytes(run)
            run.clear(); first = None
        if first is None:
            first = block
        run.extend(blocks[block]); previous = block
    if run:
        yield (DATA_FIRST + first) * BS, bytes(run)

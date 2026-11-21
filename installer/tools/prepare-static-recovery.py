"""Rebuild the supported static recovery base for legacy system-as-root boot."""
from pathlib import Path
import argparse
import gzip
import hashlib
import struct
import subprocess
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import android_compat as compat

SOURCE_SHA256 = '4a0ad6f32906dfb607f21723f5bfb53a6b7a38ec82d461ebf97375c45b400354'

def rebuild(source, destination):
    source, destination = Path(source).resolve(), Path(destination).resolve()
    compat.require(source != destination and not destination.exists(), '输出必须是尚不存在的新文件')
    image = source.read_bytes()
    compat.require(compat.sha(image) == SOURCE_SHA256, '原始静态 TWRP SHA256 不匹配')
    parts = compat.boot_parts(image)
    compat.require(parts['version'] == 1, '仅支持已核验的 v1 静态镜像')
    decoder = zlib.decompressobj(31)
    original = decoder.decompress(parts['kernel']) + decoder.flush()
    dtbs = decoder.unused_data
    compat.require(decoder.eof and dtbs[:4] == bytes.fromhex('d00dfeed'), '内核压缩或追加 DTB 格式异常')
    before, after = b'skip_initramfs\0', b'want_initramfs\0'
    compat.require(original.count(before) == 1 and after not in original, '内核参数注册字符串不唯一')
    updated = original.replace(before, after)
    compressed = bytearray(gzip.compress(updated, compresslevel=9, mtime=0))
    # Python versions differ in gzip's OS byte when delegating mtime=0 to zlib.
    compressed[9] = 255
    kernel = bytes(compressed) + dtbs
    header = bytearray(parts['header'])
    struct.pack_into('<I', header, 8, len(kernel))
    align = lambda n: (n+4095)//4096*4096
    struct.pack_into('<Q', header, 1636, 4096+align(len(kernel))+align(len(parts['ramdisk'])))
    digest = hashlib.sha1()
    for data in (kernel, parts['ramdisk'], b'', parts['dtbo']):
        digest.update(data)
        digest.update(struct.pack('<I', len(data)))
    header[576:608] = digest.digest() + bytes(12)
    body = bytes(header)
    for data in (kernel, parts['ramdisk'], parts['dtbo'], parts['dtb']):
        body += data + bytes(-len(data) % 4096)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(body)
    subprocess.run([sys.executable, '-B', '-X', 'utf8', str(ROOT/'tools/avbtool.py'),
                    'add_hash_footer', '--image', str(destination), '--partition_name', 'recovery',
                    '--partition_size', str(64*1024**2), '--algorithm', 'NONE',
                    '--hash_algorithm', 'sha256', '--salt', compat.sha(body)],
                   check=True, capture_output=True, timeout=60)
    rebuilt = compat.boot_parts(destination.read_bytes())
    compat.require(all(rebuilt[name] == parts[name] for name in ('ramdisk', 'dtbo', 'dtb')), '重建后恢复组件发生非预期改动')
    roundtrip = zlib.decompressobj(31)
    raw = roundtrip.decompress(rebuilt['kernel']) + roundtrip.flush()
    compat.require(raw == updated and roundtrip.unused_data == dtbs, '重建后的内核回读不一致')
    print(compat.sha(destination.read_bytes()) + '  ' + destination.name)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    rebuild(args.source, args.output)

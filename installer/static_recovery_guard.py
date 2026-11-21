"""Reversible vendor recovery guard for a positively identified static ROM.

All planning is read-only. Writes are performed by the installer's existing
allowlisted, read-back-verified transport after its user confirmation.
Dynamic layouts return before any partition access.
"""
import base64
import hashlib
import json
import re
import struct
import zlib

from android_compat import Ext4Reader

BS = 4096
VENDOR = '/dev/block/by-name/vendor'
VBMETA = '/dev/block/by-name/vbmeta'
SCRIPT = 'bin/install-recovery.sh'
SERVICE = 'etc/init/vendor_flash_recovery.rc'
MARKER = b'#!/vendor/bin/sh\n# HackiOS static recovery guard v1\n# '


def need(value, message):
    if not value:
        raise RuntimeError('静态 recovery 防覆盖：' + message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encode(data):
    return base64.b64encode(data).decode('ascii')


def decode(data):
    return base64.b64decode(data, validate=True)


def vbmeta_flags(data):
    need(len(data) >= BS and data[:4] == b'AVB0', 'vbmeta 头不受支持')
    major, minor = struct.unpack_from('>II', data, 4)
    auth, auxiliary = struct.unpack_from('>QQ', data, 12)
    flags = struct.unpack_from('>I', data, 120)[0]
    need(major == 1 and minor <= 2 and 256 + auth + auxiliary <= len(data), 'vbmeta 长度或版本异常')
    need(flags & ~3 == 0, 'vbmeta 含未知校验标志')
    return flags


def with_flags(data, flags):
    result = bytearray(data)
    struct.pack_into('>I', result, 120, flags)
    return bytes(result)


def original_script(data):
    """Accept only the stock applypatch recovery reconstruction pattern."""
    need(data.startswith(b'#!/vendor/bin/sh\nif ! applypatch --check EMMC:'), '官方恢复脚本格式尚未支持')
    need(b'\0' not in data and len(data) <= BS, '官方恢复脚本过大或含二进制内容')
    target = re.findall(rb'--target EMMC:/dev/block/bootdevice/by-name/recovery\$\(getprop ro.boot.slot_suffix\):67108864:([0-9a-f]{40})', data)
    check = re.findall(rb'--check EMMC:/dev/block/bootdevice/by-name/recovery\$\(getprop ro.boot.slot_suffix\):67108864:([0-9a-f]{40})', data)
    need(len(target) == len(check) == 1 and target == check, '恢复目标或校验不受支持')
    need(b'--patch /vendor/recovery-from-boot.p' in data and
         re.search(rb'--source EMMC:/dev/block/bootdevice/by-name/boot\$\(getprop ro.boot.slot_suffix\):[0-9]+:[0-9a-f]{40}', data),
         '恢复源或补丁路径不受支持')


def marker_script(original, vbmeta):
    metadata = dict(v=1, s=sha(original), c=encode(zlib.compress(original, 9)),
                    b=sha(vbmeta), f=vbmeta_flags(vbmeta))
    header = MARKER + json.dumps(metadata, sort_keys=True, separators=(',', ':')).encode('ascii') + b'\nexit 0\n'
    need(len(header) <= len(original), '原脚本没有足够空间保存可逆备份，未写入')
    return header + b' ' * (len(original) - len(header))


def unpack_marker(data):
    need(data.startswith(MARKER), '防覆盖标记不受支持')
    lines = data[len(MARKER):].split(b'\n', 1)
    try:
        metadata = json.loads(lines[0])
        need(metadata.get('v') == 1, '备份标记版本不受支持')
        decoder = zlib.decompressobj()
        original = decoder.decompress(decode(metadata['c']), BS + 1)
        need(len(original) <= BS and decoder.eof and not decoder.unused_data and not decoder.unconsumed_tail,
             '脚本备份压缩内容异常')
        need(sha(original) == metadata['s'] and len(original) == len(data), '脚本内嵌备份校验失败')
        original_script(original)
        need(lines[1] == b'exit 0\n' + b' ' * (len(lines[1]) - 7), '防覆盖脚本内容被修改')
        need(metadata['f'] in (0, 1, 2, 3) and
             re.fullmatch('[0-9a-f]{64}', metadata['b']), 'vbmeta 备份标记无效')
        return original, metadata
    except (ValueError, KeyError, TypeError, zlib.error) as error:
        raise RuntimeError('静态 recovery 防覆盖：备份标记损坏') from error


class VendorReader:
    def __init__(self, read, size):
        self.raw_read = read
        self.size = size
        self.cache = {}
        self.fs = Ext4Reader(self.read)
        need(self.fs.bs == BS, '仅支持 4096 字节块的 ext4 vendor')
        # A readonly vendor with an unreplayed journal must never be edited raw.
        state = struct.unpack_from('<H', self.fs.sb, 58)[0]
        incompatible = struct.unpack_from('<I', self.fs.sb, 96)[0]
        need(state & 1 and not incompatible & (4 | 0x8000 | 0x10000), 'vendor 未干净卸载、加密或含 inline_data')

    def read(self, offset, count):
        need(0 <= offset and 0 < count <= 4 * 1024**2 and offset + count <= self.size, 'vendor 读取越界')
        first = offset // BS * BS
        last = (offset + count + BS - 1) // BS * BS
        key = first, last
        if key not in self.cache:
            raw = self.raw_read(VENDOR, first, last - first)
            need(len(raw) == last - first, 'vendor 读取截断')
            self.cache[key] = raw
        return self.cache[key][offset-first:offset-first+count]

    def resolve(self, path):
        n = 2
        for part in path.split('/'):
            items = self.fs.listing(n)
            if part not in items:
                return None
            n = items[part]
        return n

    def file(self, path):
        ino = self.resolve(path)
        return None if ino is None else self.fs.contents(ino)

    def script_block(self):
        ino = self.resolve(SCRIPT)
        need(ino is not None, '恢复脚本缺失')
        inode = self.fs.inode(ino)
        mode = struct.unpack_from('<H', inode)[0]
        flags = struct.unpack_from('<I', inode, 32)[0]
        size = struct.unpack_from('<I', inode, 4)[0] + (struct.unpack_from('<I', inode, 108)[0] << 32)
        need(mode & 0xf000 == 0x8000 and mode & 0o111 and flags & 0x80000 and not flags & (0x800 | 0x10000000),
             '脚本不是支持的普通可执行 extent 文件')
        need(0 < size <= BS, '仅处理单块恢复脚本')
        extents = self.fs.extents(inode[40:100])
        need(len(extents) == 1 and extents[0][0] == 0 and extents[0][1] == 1, '脚本分配不是单个完整块')
        offset = extents[0][2] * BS
        block = self.read(offset, BS)
        return ino, offset, size, block


def patch(node, offset, before, after):
    need(len(before) == len(after) == BS and offset % BS == 0, '补丁边界异常')
    return dict(node=node, offset=offset, before=encode(before), after=encode(after),
                before_sha256=sha(before), after_sha256=sha(after))


def plan(profile, read, size, restore=False, evidence=None):
    if profile['layout'] != 'static':
        return dict(version=1, status='dynamic_unchanged', patches=[])
    need(profile.get('physical', {}).get('vendor') == 'ext4', '静态 vendor 不是支持的 ext4，未写入')
    vendor_size = size(VENDOR)
    reader = VendorReader(read, vendor_size)
    service = reader.file(SERVICE)
    script = reader.file(SCRIPT)
    if evidence is not None:
        evidence.update(layout='static', vendor_size=vendor_size,
                        service_path=SERVICE, script_path=SCRIPT,
                        service_present=service is not None, script_present=script is not None,
                        service_text=None if service is None else service.decode('utf-8','replace'),
                        script_text=None if script is None else script.decode('utf-8','replace'))
        # Only small vendor init definitions: no identifiers, NV or partition writes.
        definitions={};errors=[];visited=0;total=0
        for directory in ('etc/init','etc/init/hw'):
            try:
                ino=reader.resolve(directory)
                if ino is None:continue
                items=reader.fs.listing(ino)
                for name in sorted(items):
                    if not (name.endswith('.rc') and re.fullmatch(r'[A-Za-z0-9_.-]+',name)):continue
                    if visited>=256 or total>=1024**2:break
                    visited+=1
                    try:
                        inode=reader.fs.inode(items[name])
                        length=struct.unpack_from('<I',inode,4)[0]+(struct.unpack_from('<I',inode,108)[0]<<32)
                        if not 0<length<=65536 or total+length>1024**2:continue
                        data=reader.file(directory+'/'+name);total+=length
                        if data and b'recovery' in data.lower():definitions[directory+'/'+name]=data.decode('utf-8','replace')
                    except Exception as error:
                        errors.append(directory+'/'+name+': '+str(error)[:200])
            except Exception as error:
                errors.append(directory+': '+str(error)[:200])
        evidence['vendor_init_recovery_definitions']=definitions
        evidence['supplementary_scan_errors']=errors
        evidence['supplementary_scanned_files']=visited
    if service is None and script is None:
        return dict(version=1, status='not_present', patches=[])
    need(service is not None and script is not None,
         '官方恢复机制未匹配：'+SERVICE+(' 存在' if service is not None else ' 缺失')+'；'+
         SCRIPT+(' 存在' if script is not None else ' 缺失')+'。需核对本机 init 定义，不能直接跳过；未修改分区')
    active = [line.split('#', 1)[0].strip() for line in service.decode('utf-8').splitlines()]
    need([line for line in active if line] == ['service vendor_flash_recovery /vendor/bin/install-recovery.sh', 'class main', 'oneshot'],
         '恢复服务定义不受支持，未猜测修改')
    ino, offset, length, current_block = reader.script_block()
    guarded = script.startswith(MARKER)
    if restore and not guarded:
        original_script(script)
        return dict(version=1, status='not_installed', patches=[])
    vbmeta_size = size(VBMETA)
    need(vbmeta_size % BS == 0 and BS <= vbmeta_size <= 1024**2, 'vbmeta 分区容量异常')
    current_vbmeta = read(VBMETA, 0, vbmeta_size)
    flags = vbmeta_flags(current_vbmeta)
    if guarded:
        original, metadata = unpack_marker(script)
        original_vbmeta = with_flags(current_vbmeta, metadata['f'])
        need(sha(original_vbmeta) == metadata['b'], 'vbmeta 已更新或不匹配，拒绝盲目恢复')
        need(flags in (metadata['f'], metadata['f'] | 3), 'vbmeta 状态与备份不一致')
        # Keep the original compressed marker bytes across zlib/runtime updates.
        expected_script = script
    else:
        original = script
        original_script(original)
        original_vbmeta = current_vbmeta
        expected_script = marker_script(original, original_vbmeta)
    guarded_vbmeta = with_flags(original_vbmeta, vbmeta_flags(original_vbmeta) | 3)
    original_block = original + current_block[length:]
    guarded_block = expected_script + current_block[length:]
    vendor_patch = patch(VENDOR, offset, original_block, guarded_block)
    vbmeta_patch = patch(VBMETA, 0, original_vbmeta[:BS], guarded_vbmeta[:BS])
    # Enable the compatible verification policy before changing vendor. For
    # removal, restore vendor first, then put the original policy back.
    patches = [vbmeta_patch, vendor_patch]
    status = 'present' if guarded and current_vbmeta == guarded_vbmeta else 'apply'
    result = dict(version=1, status=status, patches=patches, vendor_size=vendor_size,
                  vendor_uuid=reader.fs.sb[104:120].hex(), vendor_inode=ino,
                  service_sha256=sha(service), script_sha256=sha(original),
                  vbmeta_original_flags=vbmeta_flags(original_vbmeta), vbmeta_guard_flags=vbmeta_flags(guarded_vbmeta),
                  vbmeta_original_sha256=sha(original_vbmeta), vbmeta_guard_sha256=sha(guarded_vbmeta))
    return reverse(result) if restore else result


def reverse(value):
    if not value['patches']:
        return dict(value)
    result = dict(value, status='apply' if value.get('status')=='restore' else 'restore')
    result['patches'] = [patch(p['node'], p['offset'], decode(p['after']), decode(p['before']))
                         for p in reversed(value['patches'])]
    result['vbmeta_original_sha256'], result['vbmeta_guard_sha256'] = value['vbmeta_guard_sha256'], value['vbmeta_original_sha256']
    return result


def validate(value, read, size):
    need(value.get('version') == 1, '方案版本不受支持')
    patches = value['patches']
    if not patches:
        return
    need(len(patches) == 2 and {p['node'] for p in patches} == {VENDOR, VBMETA}, '方案写入范围异常')
    need(size(VENDOR) == value['vendor_size'], 'vendor 大小已变化')
    sb = read(VENDOR, 0, BS)
    need(sb[1128:1144].hex() == value['vendor_uuid'], 'vendor UUID 已变化')
    for p in patches:
        before, after = decode(p['before']), decode(p['after'])
        need(len(before) == len(after) == BS and sha(before) == p['before_sha256'] and sha(after) == p['after_sha256'], '备份块校验失败')
        need(p['offset'] >= 0 and p['offset'] % BS == 0 and p['offset'] + BS <= size(p['node']), '补丁范围越界')
        need(p['node'] != VBMETA or p['offset'] == 0, 'vbmeta 补丁偏移异常')
        need(read(p['node'], p['offset'], BS) in (before, after), '当前分区与本机方案不一致')
    vbmeta = read(VBMETA, 0, size(VBMETA))
    need(sha(vbmeta) in (value['vbmeta_original_sha256'], value['vbmeta_guard_sha256']), 'vbmeta 完整内容与方案不符')


def apply(value, read, size, allow, write, progress=None):
    validate(value, read, size)
    for i, p in enumerate(value['patches']):
        after = decode(p['after'])
        if read(p['node'], p['offset'], BS) != after:
            allow(p['node'], p['offset'], p['offset'] + BS)
            write(p['node'], p['offset'], after)
        need(read(p['node'], p['offset'], BS) == after, '写后读回不一致')
        if progress:
            progress(i+1, len(value['patches']), p['node'])
    validate(value, read, size)

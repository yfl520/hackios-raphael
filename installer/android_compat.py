"""Read-only Android layout detection and matching recovery construction.

All image edits are on the computer. No partition writes occur in this module.
"""
from pathlib import Path
import gzip, hashlib, json, shlex, stat, struct, subprocess, sys

def require(ok, message):
    if not ok:
        raise RuntimeError(message)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def filesystem(data):
    require(len(data) >= 4096, '文件系统头读取不完整')
    sb = data[1024:]
    if sb[56:58] == b'\x53\xef':
        log = struct.unpack_from('<I', sb, 24)[0]
        require(log <= 6, 'ext4 块大小异常')
        blocks = struct.unpack_from('<I', sb, 4)[0]
        if struct.unpack_from('<I', sb, 96)[0] & 0x80:
            blocks |= struct.unpack_from('<I', sb, 336)[0] << 32
        return dict(type='ext4', bytes=blocks*(1024 << log), uuid=sb[104:120].hex())
    if sb[:4] == bytes.fromhex('1020f5f2'):
        log = struct.unpack_from('<I', sb, 16)[0]
        require(log == 12, 'F2FS 块大小不受支持')
        return dict(type='f2fs', bytes=struct.unpack_from('<Q', sb, 36)[0] << log,
                    uuid=sb[108:124].hex())
    if sb[:4] == bytes.fromhex('e2e1f5e0'):
        return dict(type='erofs')
    return None

def lp_geometry(data):
    """Validate an Android liblp geometry copy; do not trust partition names."""
    found = []
    for offset in (4096, 8192):
        block = data[offset:offset+4096]
        if len(block) < 52 or block[:4] != b'gDla':
            continue
        size = struct.unpack_from('<I', block, 4)[0]
        require(52 <= size <= 4096, '动态分区 geometry 长度异常')
        raw = bytearray(block[:size]); checksum = bytes(raw[8:40]);raw[8:40] = bytes(32)
        require(hashlib.sha256(raw).digest() == checksum, '动态分区 geometry 校验失败')
        maximum, slots, logical = struct.unpack_from('<III', block, 40)
        require(maximum >= 512 and maximum % 512 == 0 and 1 <= slots <= 4 and logical == 4096,
                '动态分区 geometry 参数不受支持')
        found.append((maximum, slots, logical))
    require(not found or len(set(found)) == 1, '动态分区主副 geometry 不一致')
    return found[0] if found else None

def detect_layout(read, exists):
    """read(node, offset, count) must return raw, complete bytes."""
    geometries = []
    physical = {}
    for name in ('super', 'system'):
        node = '/dev/block/by-name/'+name
        if not exists(node):
            continue
        header = read(node, 0, 12288)
        geometry = lp_geometry(header)
        if geometry:
            geometries.append(dict(node=node, geometry=geometry))
        else:
            fs = filesystem(header[:4096])
            if name == 'system' and fs and fs['type'] in ('ext4', 'erofs'):
                physical[name] = fs['type']
    require(len(geometries) <= 1, '同时检测到多个 super 容器，无法自动选择')
    if geometries:
        require('system' not in physical or geometries[0]['node'].endswith('/super'),
                'system 同时被识别为物理文件系统和 super')
        return dict(layout='dynamic', super_node=geometries[0]['node'])
    for name in ('vendor', 'product', 'system_ext', 'odm'):
        node = '/dev/block/by-name/'+name
        if exists(node):
            fs = filesystem(read(node, 0, 4096))
            if fs and fs['type'] in ('ext4', 'erofs'):
                physical[name] = fs['type']
    require('system' in physical and 'vendor' in physical,
            '未识别到完整静态分区或有效动态分区；已停止，未修改 GPT')
    return dict(layout='static', physical=physical)

class Ext4Reader:
    """Bounded ext4 reader for the small, unencrypted vendor fstab files."""
    def __init__(self, read):
        self.read = read
        self.sb = read(1024, 1024)
        require(self.sb[56:58] == b'\x53\xef', 'vendor 不是 ext4')
        log = struct.unpack_from('<I', self.sb, 24)[0]
        require(log <= 6, 'vendor ext4 块大小异常')
        self.bs = 1024 << log
        self.ipg = struct.unpack_from('<I', self.sb, 40)[0]
        self.isz = struct.unpack_from('<H', self.sb, 88)[0]
        self.ds = max(32, struct.unpack_from('<H', self.sb, 254)[0])
        require(self.ipg > 0 and 128 <= self.isz <= self.bs and 32 <= self.ds <= self.bs,
                'vendor ext4 参数异常')

    def inode(self, n):
        require(0 < n <= struct.unpack_from('<I', self.sb)[0], 'vendor inode 越界')
        group, index = divmod(n-1, self.ipg)
        gd = self.read((2 if self.bs == 1024 else 1)*self.bs+group*self.ds, self.ds)
        table = struct.unpack_from('<I', gd, 8)[0]
        if self.ds >= 64:
            table |= struct.unpack_from('<I', gd, 40)[0] << 32
        return self.read(table*self.bs+index*self.isz, self.isz)

    def extents(self, h, depth=0):
        require(len(h) >= 12, 'vendor extent 截断')
        magic, n, limit, level = struct.unpack_from('<4H', h)
        require(magic == 0xf30a and n <= limit and 12+n*12 <= len(h) and level <= 5 and depth <= 5,
                'vendor extent 参数异常')
        out = []
        for i in range(n):
            at = 12+i*12
            if level:
                _, low, hi = struct.unpack_from('<IIH', h, at)
                child = self.read((low+(hi << 32))*self.bs, self.bs)
                require(struct.unpack_from('<H', child, 6)[0] == level-1, 'vendor extent 层级错误')
                out += self.extents(child, depth+1)
            else:
                logical, length, hi, low = struct.unpack_from('<IHHI', h, at)
                require(0 < length <= 32768, 'vendor 包含未初始化 extent')
                out.append((logical, length, (hi << 32)+low))
        return out

    def contents(self, n):
        ino = self.inode(n)
        size = struct.unpack_from('<I', ino, 4)[0]+(struct.unpack_from('<I', ino, 108)[0] << 32)
        require(size <= 4*1024**2, 'vendor 目录或 fstab 超过读取上限')
        if struct.unpack_from('<H', ino)[0] & 0xf000 == 0xa000:
            raise RuntimeError('vendor fstab 路径含符号链接，需另行核验')
        require(struct.unpack_from('<I', ino, 32)[0] & 0x80000, 'vendor inode 未使用 extents')
        out = bytearray(size)
        for logical, length, physical in self.extents(ino[40:100]):
            at = logical*self.bs; count = min(length*self.bs, size-at)
            if count > 0:
                out[at:at+count] = self.read(physical*self.bs, count)
        return bytes(out)

    def listing(self, n):
        data = self.contents(n); at = 0; out = {}
        while at+8 <= len(data):
            ino, rec, nl, _ = struct.unpack_from('<IHBB', data, at)
            require(rec >= 8 and rec % 4 == 0 and at+rec <= len(data) and nl <= rec-8,
                    'vendor 目录项异常')
            if ino:
                out[data[at+8:at+8+nl].decode('utf-8')] = ino
            at += rec
        return out

    def fstabs(self):
        root = self.listing(2)
        require('etc' in root, 'vendor 缺少 etc 目录')
        return {name:self.contents(n).decode('utf-8') for name,n in self.listing(root['etc']).items()
                if name.startswith('fstab.')}

def data_entry(fstabs, actual_type=None):
    candidates = []
    for filename, text in sorted(fstabs.items()):
        for line in text.splitlines():
            fields = line.split('#', 1)[0].split()
            if len(fields) == 5 and fields[1] == '/data' and fields[2] in ('ext4', 'f2fs'):
                require(fields[0].endswith('/by-name/userdata'), 'Android Data 设备路径不受支持')
                candidates.append(tuple(fields))
    require(candidates, '没有读到原 Android fstab 的 Data 条目，禁止猜测格式化方式')
    unique = sorted(set(candidates))
    if actual_type:
        matching = [x for x in unique if x[2] == actual_type]
        require(matching, 'userdata 实际格式与原 Android fstab 不符，已停止')
        unique = matching
    require(len(unique) == 1, '原 Android Data 配置存在歧义，已停止')
    result = list(unique[0]); result[0] = '/dev/block/by-name/userdata'
    require(not any(x.startswith(('encryptable=', 'forceencrypt=')) for x in result[4].split(',')),
            '当前脚本不支持旧式整盘加密，已停止')
    return result

def unpack_cpio(data):
    out = []; at = 0
    while at+110 <= len(data):
        h = data[at:at+110]
        require(h[:6] == b'070701', '只支持 newc ramdisk')
        fields = [int(h[6+i*8:14+i*8], 16) for i in range(13)]
        size, namesize = fields[6], fields[11]
        require(0 < namesize <= 4096 and at+110+namesize <= len(data), 'ramdisk 名称越界')
        name = data[at+110:at+110+namesize-1].decode('utf-8')
        start = (at+110+namesize+3) & ~3
        require(start+size <= len(data), 'ramdisk 内容越界')
        out.append((name, fields, data[start:start+size]))
        at = (start+size+3) & ~3
        if name == 'TRAILER!!!':
            require(len({x[0] for x in out}) == len(out), 'ramdisk 存在重复路径')
            return out
    raise RuntimeError('ramdisk 缺少结束标记')

def pack_cpio(entries):
    out = bytearray()
    for name, original, data in entries:
        fields = original.copy();name = name.encode()+b'\0';fields[6] = len(data);fields[11] = len(name)
        out += b'070701'+b''.join(f'{n:08x}'.encode() for n in fields)+name
        out += bytes(-len(out) % 4);out += data;out += bytes(-len(out) % 4)
    out += bytes(-len(out) % 512)
    return bytes(out)

def boot_parts(image):
    require(image[:8] == b'ANDROID!' and len(image) >= 4096, 'TWRP Android 镜像头无效')
    ks,ka,rs,ra,ss,sa,tags,page,version,osver = struct.unpack_from('<10I', image, 8)
    require(page == 4096 and version in (1,2) and ss == 0, 'TWRP 镜像封装不受支持')
    align = lambda n:(n+page-1)//page*page
    ro = page+align(ks)
    rec, recoff, hsize = struct.unpack_from('<IQI', image, 1632)
    require(hsize == (1648 if version == 1 else 1660) and recoff == ro+align(rs), 'TWRP recovery DTBO 偏移无效')
    dtbsize = struct.unpack_from('<I', image, 1648)[0] if version == 2 else 0
    end = recoff+align(rec)+align(dtbsize)
    require(end <= len(image), 'TWRP 镜像截断')
    return dict(header=image[:page],kernel=image[page:page+ks],ramdisk=image[ro:ro+rs],
                dtbo=image[recoff:recoff+rec],dtb=image[recoff+align(rec):recoff+align(rec)+dtbsize],version=version)

def recovery_files(image):
    parts = boot_parts(image)
    entries = unpack_cpio(gzip.decompress(parts['ramdisk']))
    return {n:d for n,_,d in entries}

def matching_configs(base_files, profile=None, probe=False):
    if probe:
        fstab = ('# HackiOS read-only layout probe: no userdata/metadata/system mounts.\n'
                 '/dev/block/by-name/misc /misc emmc defaults defaults\n')
        flags = ('/boot emmc /dev/block/by-name/boot\n'
                 '/recovery emmc /dev/block/by-name/recovery\n')
        if probe == 'dynamic':
            fstab = '# Dynamic mapping probe; no userdata or metadata mounts.\n'
            # Avoid /vendor: TWRP automatically imports its fstab and could
            # otherwise recreate a Data definition before host-side validation.
            fstab += 'vendor /probe_vendor ext4 ro wait,logical\nvendor /probe_vendor erofs ro wait,logical\n'
            flags += '/probe_vendor erofs vendor flags=logical;fsflags="ro"\n'
        return fstab.encode(), flags.encode()
    entry = profile['data_entry'].copy()
    # Android-only init flags are not interpreted by TWRP; crypto options are preserved.
    options = [x for x in entry[4].split(',') if x not in ('latemount','first_stage_mount')]
    if 'formattable' not in options:
        options.append('formattable')
    entry[4] = ','.join(options)
    original = base_files['system/etc/recovery.fstab'].decode()
    lines = ['# Data policy copied from the installed Android vendor fstab.']
    if profile['layout'] == 'dynamic':
        for line in original.splitlines():
            f = line.split('#',1)[0].split()
            if len(f) == 5 and f[1] not in ('/data','/metadata','/misc'):
                lines.append(line)
    else:
        for name, fs in sorted(profile['physical'].items()):
            point = '/system_root' if name == 'system' else '/'+name
            lines.append(f'/dev/block/by-name/{name} {point} {fs} ro wait')
    lines += ['/dev/block/by-name/metadata /metadata ext4 noatime,nosuid,nodev,discard wait,check,formattable',
              ' '.join(entry), '/dev/block/by-name/misc /misc emmc defaults defaults']
    flags = []
    for line in base_files['system/etc/twrp.flags'].decode().splitlines():
        f = line.split('#',1)[0].split()
        if f and f[0] in ('/data','/metadata'):
            continue
        if profile['layout'] == 'static' and f and f[0] in ('/system','/system_root','/vendor','/product','/system_ext','/odm'):
            continue
        flags.append(line)
    # Use one definition per physical partition. TWRP handles system-as-root itself.
    if profile['layout'] == 'static':
        for name,fs in sorted(profile['physical'].items()):
            point = '/system_root' if name == 'system' else '/'+name
            flags.append(f'{point} {fs} /dev/block/by-name/{name} flags=display="{name}";fsflags="ro"')
    flags += ['/metadata ext4 /dev/block/by-name/metadata flags=backup=1;wipeingui']
    return ('\n'.join(lines)+'\n').encode(), ('\n'.join(flags)+'\n').encode()

def build_recovery(base, destination, avbtool, profile=None, probe=False):
    """Keep the selected TWRP's kernel, DTBO and DTB byte-for-byte."""
    image = Path(base).read_bytes();parts = boot_parts(image)
    entries = unpack_cpio(gzip.decompress(parts['ramdisk']))
    files = {n:d for n,_,d in entries}
    fstab, flags = matching_configs(files, profile, probe)
    changed = {'system/etc/recovery.fstab':fstab,'system/etc/twrp.flags':flags}
    require(all(name in files for name in changed), 'TWRP 缺少所需配置文件')
    updated = [(n,f,changed.get(n,d)) for n,f,d in entries]
    compressed = bytearray(gzip.compress(pack_cpio(updated), compresslevel=6, mtime=0))
    compressed[9] = 255  # Stable gzip OS byte across host and bundled Python.
    ramdisk = bytes(compressed)
    header = bytearray(parts['header']);struct.pack_into('<I',header,16,len(ramdisk))
    align = lambda n:(n+4095)//4096*4096
    struct.pack_into('<Q', header, 1636, 4096+align(len(parts['kernel']))+align(len(ramdisk)))
    digest = hashlib.sha1()
    # mkbootimg v1 hashes kernel, ramdisk, empty second and recovery_dtbo; v2 adds DTB.
    segments = [parts['kernel'],ramdisk,b'',parts['dtbo']]
    if parts['version'] == 2:
        segments.append(parts['dtb'])
    for data in segments:
        digest.update(data);digest.update(struct.pack('<I',len(data)))
    header[576:608] = digest.digest()+bytes(12)
    body = bytes(header)
    for data in (parts['kernel'],ramdisk,parts['dtbo'],parts['dtb']):
        body += data+bytes(-len(data)%4096)
    destination = Path(destination);destination.parent.mkdir(parents=True,exist_ok=True)
    destination.write_bytes(body)
    # A valid hash footer without a signing key is appropriate to the required unlocked BL.
    # Never retain the source image's now-invalid signature after changing its ramdisk.
    command = [sys.executable,'-B','-X','utf8',str(avbtool),'add_hash_footer','--image',str(destination),
               '--partition_name','recovery','--partition_size',str(64*1024**2),
               '--algorithm','NONE','--hash_algorithm','sha256','--salt',sha(body)]
    subprocess.run(command,check=True,capture_output=True,timeout=60)
    result = destination.read_bytes();new = boot_parts(result)
    require(len(result)==64*1024**2 and result[-64:-60]==b'AVBf', '生成的 recovery 长度或 AVB 封装错误')
    require(all(new[k]==parts[k] for k in ('kernel','dtbo','dtb')), '生成时修改了 TWRP 硬件组件')
    newfiles = recovery_files(result)
    require(set(newfiles)==set(files) and all(newfiles[n]==changed.get(n,d) for n,d in files.items()),
            'ramdisk 回读发现非预期改动')
    return dict(image_sha256=sha(result),fstab_sha256=sha(fstab),flags_sha256=sha(flags),
                base_sha256=sha(image),probe=probe)

def validate_formatted(header, expected, partition_bytes, before=None):
    result = filesystem(header)
    require(result and result['type'] == expected, '格式化后的 userdata 文件系统不符合原 Android 配置')
    require(0 < result['bytes'] <= partition_bytes, '格式化后的 userdata 文件系统越过分区边界')
    require(result['uuid'] != '00'*16, '格式化后的 userdata UUID 无效')
    if before and before.get('uuid'):
        require(result['uuid'] != before['uuid'], 'userdata UUID 未变化，无法确认格式化成功')
    return result

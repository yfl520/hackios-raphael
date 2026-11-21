"""K20 Pro persistent recovery installer. All mutations follow checked plans and local confirmation."""
from pathlib import Path
import argparse,datetime,gzip,hashlib,io,json,os,re,secrets,shlex,shutil,struct,subprocess,sys,time,zlib
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
from disk import *
from console_ui import console,Task,command_label
import android_compat as android
import static_recovery_guard as static_guard
from data_volume import initial_extents
import device_resources as wireless
CHUNK=32*1024**2
DARWIN='/dev/block/by-name/darwin'
RECOVERY='/dev/block/by-name/recovery'
SAFE_BACKUPS=('boot','recovery','dtbo','vbmeta','misc','devinfo','persist','persistbak','metadata')
COMMAND_LOG=None
ACTIVE_PHONE=None
RECOVERY_READY_PROBE=('getprop ro.product.device; getprop ro.twrp.version; '
    'getprop sys.boot_completed; getprop init.svc.adbd; '
    'getprop sys.usb.config; getprop sys.usb.state; '
    'getprop init.svc_debug_pid.adbd; cat /proc/sys/kernel/random/boot_id')

def record_command(value):
    global COMMAND_LOG
    if COMMAND_LOG is None:
        stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
        directory=ROOT/'session-private'/'runlogs'/stamp
        directory.mkdir(parents=True,exist_ok=False);COMMAND_LOG=directory/'commands.jsonl'
        say('本轮完整命令记录：'+str(COMMAND_LOG))
    value['time_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    with COMMAND_LOG.open('a',encoding='utf-8') as stream:
        stream.write(json.dumps(value,ensure_ascii=False)+'\n')

class UI:
    reset='\x1b[0m'; bold='\x1b[1m'; cyan='\x1b[36m'; green='\x1b[32m'; yellow='\x1b[33m'; red='\x1b[31m'; blue='\x1b[34m'; dim='\x1b[2m'

def _enable_colors():
    console.enable()

def say(text='',color=None):
    console.write(text,color)

def section(title):
    say('\n  ── '+title+' '+ '─'*12,UI.bold+UI.cyan)

def stage(number,total,label):
    say(f'\n  {number:02d} / {total:02d}  '+label,UI.bold+UI.cyan)

def ok(text):say('✓ '+text,UI.green)
def warn(text):say('! '+text,UI.yellow)
def fail(text):say('✗ '+text,UI.red)

def confirm(message,token=None):
    section('请确认操作')
    warn(message)
    answer=input('继续执行？[Y/N]：').strip().upper()
    need(answer=='Y','操作已取消，未继续执行')

def run(args,timeout=30,binary=False):
    argv=[str(x) for x in args]
    record_command(dict(event='start',args=argv,timeout=timeout))
    try:
        with Task(command_label(args),delay=.6):
            p=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout)
            record_command(dict(event='finish',args=argv,returncode=p.returncode,
                stdout_bytes=len(p.stdout),stdout_sha256=digest(p.stdout),
                stdout=None if binary else p.stdout.decode('utf-8','replace'),stderr=p.stderr.decode('utf-8','replace')))
            if p.returncode:
                detail=(p.stderr if binary else p.stdout+p.stderr).decode('utf-8','replace')[-3000:]
                if '5037' in detail or 'cannot connect to daemon' in detail or 'daemon still not running' in detail:
                    detail='电脑本地 ADB 服务不可用，当前操作已停止。关闭其他手机助手/模拟器，检查安全软件和本机 5037 端口后重试；若首次安装已开始写入，保留原目录并用选项 8 续装，不要重新首次安装。\n'+detail
                raise RuntimeError(detail or '设备命令失败，退出码 '+str(p.returncode))
    except subprocess.TimeoutExpired as e:
        record_command(dict(event='timeout',args=argv,stdout_bytes=len(e.stdout or b''),stderr=(e.stderr or b'').decode('utf-8','replace')))
        raise RuntimeError('命令超时，已停止；不会自动重启或继续刷写：'+str(args[0])) from e
    return p.stdout if binary else (p.stdout+p.stderr).decode('utf-8','replace')

def legacy_shell_script(cmd,token):
    """Frame raw exec-out without requiring adbd's shell_v2 protocol.

    stderr is staged separately; stdout streams directly (including binary).
    Prefer recovery RAM /tmp: inherited TMPDIR can be /data/local/tmp,
    where our own open stderr file would prevent unmounting Android data.
    A random prefix/footer proves completion and carries the remote exit code.
    No user command, especially a write, is retried by this transport.
    """
    need(re.fullmatch('[0-9a-f]{32}',token),'内部命令标识错误')
    return f'''umask 077
if [ -d /tmp ] && [ -w /tmp ]; then
    _hackios_tmp=/tmp
elif [ -n "$TMPDIR" ] && [ -d "$TMPDIR" ] && [ -w "$TMPDIR" ]; then
    _hackios_tmp=$TMPDIR
elif [ -d /data/local/tmp ] && [ -w /data/local/tmp ]; then
    _hackios_tmp=/data/local/tmp
else
    printf '%s\\n' 'HackiOS: no writable command temporary directory' >&2
    exit 125
fi
_hackios_dir="$_hackios_tmp/hackios-adb-{token}"
mkdir "$_hackios_dir" || exit 125
trap 'rm -f "$_hackios_dir/stderr"; rmdir "$_hackios_dir"' 0
: >"$_hackios_dir/stderr" || exit 125
printf '%s\\n' 'HACKIOS-BEGIN-{token}'
(
{cmd}
) 2>"$_hackios_dir/stderr"
_hackios_status=$?
_hackios_count=$(wc -c <"$_hackios_dir/stderr") || exit 125
cat "$_hackios_dir/stderr" || exit 125
printf '\\nHACKIOS-END-{token}:%s:%s\\n' "$_hackios_status" "$_hackios_count"
'''

def decode_legacy_shell(raw,token,binary=False):
    prefix=('HACKIOS-BEGIN-'+token+'\n').encode('ascii')
    suffix=re.search(rb'\nHACKIOS-END-'+token.encode('ascii')+rb':([0-9]{1,3}):[ \t]*([0-9]+)[ \t]*\n\Z',raw)
    if not (raw.startswith(prefix) and suffix is not None and suffix.start()>=len(prefix)):
        record_command(dict(event='remote_shell_incomplete',transport='exec-out-framed',
            received_bytes=len(raw),received_sha256=digest(raw),
            begin_marker_present=raw.startswith(prefix),end_marker_present=suffix is not None))
        raise RuntimeError('ADB 命令返回不完整或混入额外输出（收到 '+str(len(raw))+
            ' 字节，'+('完成标记缺失' if suffix is None else '起始标记异常')+
            '），未采用数据；已停止，不会自动重放写入')
    status=int(suffix[1]);n=int(suffix[2]);payload=raw[len(prefix):suffix.start()]
    need(status<=255 and n<=len(payload),'ADB 命令返回的状态或错误长度不正确，未采用数据')
    stdout,stderr=(payload[:-n],payload[-n:]) if n else (payload,b'')
    record_command(dict(event='remote_shell_finish',transport='exec-out-framed',returncode=status,
        stdout_bytes=len(stdout),stdout_sha256=digest(stdout),
        stdout=None if binary else stdout.decode('utf-8','replace'),stderr=stderr.decode('utf-8','replace')))
    if status:
        detail=(stderr if binary else stdout+stderr).decode('utf-8','replace')[-3000:]
        raise RuntimeError('设备命令失败，退出码 '+str(status)+(('：'+detail) if detail else ''))
    return stdout if binary else (stdout+stderr).decode('utf-8','replace')

def hash_local(path,progress=None,base=0):
    h=hashlib.sha256();done=0
    with path.open('rb') as stream:
        while data:=stream.read(4*1024**2):
            h.update(data);done+=len(data)
            if progress:progress.update(base+done,path.name)
    return h.hexdigest()

def file_sha(path):
    with Task('校验本地文件',path.stat().st_size,'bytes',delay=.6) as progress:
        return hash_local(path,progress)

def verify_package():
    manifest=json.loads((ROOT/'manifest.json').read_text(encoding='utf-8'));expected=set()
    need(manifest['version']==1,'清单版本不支持')
    verified=0
    with Task('检查安装包完整性',sum(e['bytes'] for e in manifest['files']),'bytes') as progress:
        for i,e in enumerate(manifest['files']):
            path=ROOT/e['file'];need(path.resolve().is_relative_to(ROOT.resolve()),'清单路径越界')
            need(e['file'] not in expected,'清单有重复路径');expected.add(e['file'])
            need(path.is_file() and path.stat().st_size==e['bytes'],'文件缺失或长度不正确：'+e['file'])
            need(hash_local(path,progress,verified)==e['sha256'],'文件哈希不正确：'+e['file'])
            verified+=e['bytes']
    actual={p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*') if p.is_file() and
        'session-private' not in p.relative_to(ROOT).parts and '__pycache__' not in p.relative_to(ROOT).parts and p.name!='manifest.json'}
    need(actual==expected,'目录包含缺失或额外文件，请重新完整解压到空文件夹')
    config=json.loads((ROOT/'config.json').read_text(encoding='utf-8'))
    need(config['device']=='raphael' and config['minimum_darwin_blocks']==5242875,'配置与引导器不匹配')
    image=(ROOT/'images/ios-recovery.img').read_bytes()
    need(image[:8]==b'ANDROID!' and len(image)==64*1024**2 and image[-64:-60]==b'AVBf','recovery 封装不完整')
    resources=ROOT/'images/ios-boot-resources.bin';need(resources.stat().st_size==RESOURCE_BYTES,'启动资源长度错误')
    with resources.open('rb') as f:head=f.read(4096)
    need(head[:8]==b'RAPHIOS1' and struct.unpack_from('<Q',head,16)[0]==RESOURCE_BYTES,'启动资源头错误')
    fields=wireless.HEADER.unpack_from(head)
    need(fields[1:3]==(2,4096) and fields[-1]==1,'当前引导器只支持本包的每机资源版本')
    checked=bytearray(head);struct.pack_into('<I',checked,64,0)
    need(zlib.crc32(checked)==fields[-2],'启动资源头校验失败')
    layout=json.loads((ROOT/'images/device-resource-map.json').read_text(encoding='utf-8'))
    wireless.geometry(layout,fields)
    need(config['system_bytes']==SYSTEM_BYTES and config['resource_bytes']==RESOURCE_BYTES,'配置与镜像几何参数不匹配')
    chunks=list(sparse_chunks(ROOT/'images/data-initial.simg'))
    need(chunks and all(DATA_FIRST*BS<=offset and offset+len(data)<=DARWIN_BLOCKS*BS for offset,data in chunks),'数据卷初始化镜像越界')
    for name in ('twrp-static-base.img','twrp-original.img','twrp-probe.img','twrp-dynamic-probe.img'):
        source=(ROOT/'images'/name).read_bytes()
        need(len(source)==64*1024**2 and source[-64:-60]==b'AVBf','TWRP 镜像封装不完整：'+name)
        files=android.recovery_files(source)
        properties=files.get('prop.default',b'').decode('utf-8','replace')
        need(any(line in properties.splitlines() for line in ('ro.product.device=raphael','ro.product.system.device=raphael')),'TWRP 机型不匹配：'+name)
    say('包内 '+str(len(expected))+' 个文件完整性检查通过。')
    return config

class Phone:
    def __init__(self):
        global ACTIVE_PHONE
        ACTIVE_PHONE=self
        self.adb=ROOT/'tools/platform-tools/adb.exe';self.fb=ROOT/'tools/platform-tools/fastboot.exe'
        self.serial=None;self.allowed={};self.session=None
        self.android_profile=None;self.recovery_image=None;self.recovery_checks=None
        self.recovery_boot_id=None
        self.adb_server_started=False

    def start_adb_server(self):
        if not self.adb_server_started:
            # -H (even 127.0.0.1) and -L tcp:127.0.0.1:5037 disable
            # adb's local auto-start. A hostless socket explicitly starts the
            # local daemon and overrides inherited remote server settings.
            run([self.adb,'-L','tcp:5037','start-server'],45)
            self.adb_server_started=True

    def adb_devices(self):
        self.start_adb_server()
        out=run([self.adb,'-H','127.0.0.1','-P','5037','devices'])
        return [line.split()[:2] for line in out.splitlines() if '\t' in line]

    def fastboot_devices(self):
        out=run([self.fb,'devices'])
        return [line.split()[0] for line in out.splitlines() if line.strip() and line.split()[-1]=='fastboot']

    def shell(self,cmd,timeout=45,binary=False):
        need(self.serial is not None,'尚未绑定手机')
        token=secrets.token_hex(16)
        script=legacy_shell_script(cmd,token)
        # exec-out keeps its first command argument verbatim, but escapes any
        # subsequent arguments. Pass one complete command, quoted exactly once.
        raw=run([self.adb,'-H','127.0.0.1','-P','5037','-s',self.serial,'exec-out',
                 'sh -c '+shlex.quote(script)],timeout,True)
        return decode_legacy_shell(raw,token,binary)

    def adb_command(self,*args,timeout=45):return run([self.adb,'-H','127.0.0.1','-P','5037','-s',self.serial,*args],timeout)
    def fb_command(self,*args,timeout=45):return run([self.fb,'-s',self.serial,*args],timeout)

    def wait_adb(self,timeout=90):
        with Task('等待手机进入 ADB / TWRP'):
            until=time.monotonic()+timeout
            while time.monotonic()<until:
                devices=self.adb_devices()
                need(len(devices)<=1,'请只连接一台 Android 手机')
                if devices and devices[0][0]==self.serial and devices[0][1] in ('device','recovery'):return
                time.sleep(1)
            raise RuntimeError('未等到同一手机的 ADB；请检查手机画面和 USB 驱动，不会自动重刷')

    def wait_recovery_ready(self,timeout=90,settle=5,new_boot=False):
        # Recovery first exposes adb-only USB, then restarts adbd for MTP+ADB.
        # Only these fixed read-only probes may be repeated during that window.
        # Never call shell with a write/unmount/reboot command from this loop.
        expected_boot=None if new_boot else self.recovery_boot_id
        deadline=time.monotonic()+timeout;stable_since=None;signature=None
        last_error='尚未出现同一手机的 recovery';announced=False
        with Task('等待 TWRP 的 USB 与 ADB 初始化稳定'):
            while time.monotonic()<deadline:
                try:
                    devices=self.adb_devices()
                except RuntimeError as exc:
                    devices=[];last_error=str(exc)
                need(len(devices)<=1,'请只连接一台 Android 手机')
                need(not devices or devices[0][0]==self.serial,'检测到另一台手机，停止等待，不继续操作')
                fields=None
                if devices and devices[0][1] in ('device','recovery'):
                    try:
                        fields=self.shell(RECOVERY_READY_PROBE,timeout=10).splitlines()
                    except RuntimeError as exc:
                        last_error=str(exc)
                        record_command(dict(event='recovery_probe_interrupted',error=last_error,
                            retry_scope='fixed_read_only_startup_probe'))
                        if not announced:
                            say('TWRP 启动连接尚未稳定，等待同一手机重新连接；本阶段未执行分区写入。')
                            announced=True
                if fields is not None and len(fields)==8:
                    product,version,completed,adbd,config,state,pid,boot=fields
                    need(not product or product=='raphael','临时 recovery 机型不匹配')
                    need(version or completed!='1','临时 TWRP 未启动，手机进入了 Android；已停止，未继续写入。请保留本次命令记录。')
                    boot_ok=re.fullmatch(r'[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}',boot)
                    if boot_ok and expected_boot:
                        need(boot==expected_boot,'等待连接时手机已重新启动，停止操作；请保留原会话后重新运行')
                    # Empty optional USB/service properties on older recoveries
                    # are allowed; when present, they must describe a ready link.
                    usb_ok=(not config or 'adb' in config.split(',')) and (not config or not state or config==state)
                    ready=product=='raphael' and bool(version) and boot_ok and adbd in ('','running') and usb_ok
                    if ready:
                        current=(version,adbd,config,state,pid,boot)
                        if current!=signature:
                            signature=current;stable_since=time.monotonic()
                        if time.monotonic()-stable_since>=settle:
                            self.recovery_boot_id=boot
                            record_command(dict(event='recovery_ready',version=version,
                                usb_config=config,usb_state=state,settle_seconds=settle,
                                read_only_probe=True))
                            return
                    else:
                        signature=None;stable_since=None;last_error='TWRP 或 USB 配置尚未就绪'
                else:
                    signature=None;stable_since=None
                    if fields is not None:last_error='TWRP 启动探测返回字段不完整'
                time.sleep(1)
        raise RuntimeError('等待 TWRP 启动连接稳定超时；未继续分区操作。最后状态：'+last_error)

    def wait_fastboot(self,timeout=90):
        with Task('等待手机进入 FASTBOOT'):
            until=time.monotonic()+timeout
            while time.monotonic()<until:
                devices=self.fastboot_devices();need(len(devices)<=1,'请只连接一台手机')
                if devices==[self.serial]:return
                time.sleep(1)
            raise RuntimeError('未等到同一手机的 FASTBOOT；请检查手机画面和驱动')

    def check_fastboot(self):
        need(re.search(r'product:\s*raphael\b',self.fb_command('getvar','product')),'仅支持 raphael / Redmi K20 Pro')
        need(re.search(r'Device unlocked:\s*true',self.fb_command('oem','device-info'),re.I),'Bootloader 未解锁，程序不会自动解锁')

    def boot_twrp(self):
        if self.recovery_image:
            self._boot_twrp_image(self.recovery_image)
            self.verify_android_profile()
            return
        self._boot_twrp_image(ROOT/'images/twrp-probe.img')
        self.prepare_android_recovery()

    def _boot_twrp_image(self,image):
        self.check_fastboot()
        say('临时启动 '+image.name+'；此步骤不刷 recovery。')
        say(self.fb_command('boot',image,timeout=90))
        self.wait_recovery_ready(new_boot=True)
        self.check_recovery()

    def block_exists(self,node):
        need(re.fullmatch(r'/dev/block/(?:by-name/[A-Za-z0-9_]+|mapper/vendor(?:_[ab])?)',node),'探测节点不受支持')
        return self.shell('if [ -b '+shlex.quote(node)+' ]; then echo yes; fi').strip()=='yes'

    def vendor_fstabs(self,layout):
        if layout['layout']=='static':
            node='/dev/block/by-name/vendor'
        else:
            until=time.monotonic()+45
            with Task('等待动态 vendor 映射就绪'):
                while True:
                    choices=[n for n in ('/dev/block/mapper/vendor','/dev/block/mapper/vendor_a','/dev/block/mapper/vendor_b') if self.block_exists(n)]
                    if choices or time.monotonic()>=until:break
                    time.sleep(1)
            need(len(choices)==1,'动态 vendor 映射缺失或存在歧义；已停止，未修改 GPT')
            node=choices[0]
        n=self.size(node);cache={}
        def read(offset,count):
            need(0<=offset and 0<count<=4*1024**2 and offset+count<=n,'vendor 只读范围越界')
            begin=offset//BS*BS;end=(offset+count+BS-1)//BS*BS
            key=(begin,end)
            if key not in cache:cache[key]=self.read(node,begin,end-begin)
            return cache[key][offset-begin:offset-begin+count]
        fs=android.filesystem(read(0,BS))
        need(fs and fs['type'] in ('ext4','erofs'),'vendor 文件系统不受支持；不猜测 Android 配置')
        if fs['type']=='ext4':return android.Ext4Reader(read).fstabs()
        # EROFS is immutable. Use a private read-only mount, then always release it.
        point='/tmp/hackios-vendor-readonly'
        self.shell('mkdir -p '+point+' && mount -t erofs -o ro '+shlex.quote(node)+' '+point)
        try:
            listing=self.shell('find '+point+'/etc -maxdepth 1 -type f -name '+shlex.quote('fstab.*'))
            paths=listing.splitlines();need(paths and all(re.fullmatch(point+r'/etc/fstab\.[A-Za-z0-9_.-]+',p) for p in paths),'EROFS vendor fstab 路径不受支持')
            return {Path(p).name:self.shell('cat '+shlex.quote(p)) for p in paths}
        finally:self.shell('umount '+point)

    def prepare_android_recovery(self):
        # Detect nonstandard multiboot before preparing or replacing an entry.
        standard_layout_only(snapshot(self))
        with Task('只读识别 Android 分区布局与文件系统'):
            layout=android.detect_layout(self.read,self.block_exists)
        label='官方 / 静态分区' if layout['layout']=='static' else '动态分区'
        ok('已识别 '+label)
        # Dynamic vendor needs liblp mappings. The probe retains logical system
        # mapping definition but has no Data/metadata entries or /vendor import.
        if layout['layout']=='dynamic':
            self.adb_command('reboot','bootloader');self.wait_fastboot()
            self._boot_twrp_image(ROOT/'images/twrp-dynamic-probe.img')
        with Task('读取原 Android 的 userdata 格式与加密配置'):
            fstabs=self.vendor_fstabs(layout)
            current=android.filesystem(self.read('/dev/block/by-name/userdata',0,BS))
            entry=android.data_entry(fstabs,current['type'] if current else None)
        base=ROOT/('images/twrp-static-base.img' if layout['layout']=='static' else 'images/twrp-original.img')
        profile=dict(**layout,data_entry=entry,vendor_fstabs=fstabs,
                     vendor_fstabs_sha256=android.sha(json.dumps(fstabs,sort_keys=True).encode()),
                     base_sha256=digest(base.read_bytes()))
        directory=ROOT/'session-private'/'recovery-cache'/datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
        directory.mkdir(parents=True,exist_ok=False)
        image=directory/'twrp-matched.img'
        with Task('准备匹配的 TWRP，保留对应内核并校验完整封装'):
            checks=android.build_recovery(base,image,ROOT/'tools/avbtool.py',profile)
        self.android_profile=profile;self.recovery_image=image;self.recovery_checks=checks
        (directory/'profile.json').write_text(json.dumps(dict(android=profile,checks=checks),ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        say('Android Data：'+entry[2]+'；加密配置来自原 ROM。')
        self.adb_command('reboot','bootloader');self.wait_fastboot()
        self._boot_twrp_image(image)
        self.verify_android_profile()

    def verify_android_profile(self):
        need(self.android_profile is not None and self.recovery_checks is not None,'缺少 Android 兼容性检查结果')
        for path,key in (('/etc/recovery.fstab','fstab_sha256'),('/etc/twrp.flags','flags_sha256')):
            need(digest(self.shell('cat '+path,binary=True))==self.recovery_checks[key],'运行中的 TWRP 配置与本次方案不一致')
        # Processing this CLI request also waits for the recovery command worker.
        self.shell('twrp unmount data',90)
        self.check_recovery()
        # TWRP may import vendor fstab after reading recovery.fstab. Check its
        # effective Data policy as well as the files copied into the ramdisk.
        log=self.shell('cat /tmp/recovery.log')
        if COMMAND_LOG:(COMMAND_LOG.parent/'twrp-current.log').write_text(log,encoding='utf-8')
        section=re.search(r'(?ms)^/data \|[^\n]*\n(.*?)(?=^/[^\n]*\||\Z)',log)
        actual=re.search(r'Fstab_File_System:\s*(\w+)',section[1]) if section else None
        need(actual and actual[1]==self.android_profile['data_entry'][2],
             'TWRP 实际加载的 Data 格式与原 Android 不一致，停止操作')

    def preserve_failure_log(self):
        # Preserve read-only evidence for recovery failures or unexpected Android.
        devices=self.adb_devices() if self.serial else []
        if self.serial and [self.serial,'recovery'] in devices:
            log=self.shell('cat /tmp/recovery.log',timeout=10)
            if COMMAND_LOG:(COMMAND_LOG.parent/'twrp-failure.log').write_text(log,encoding='utf-8')
        elif self.serial and [self.serial,'device'] in devices:
            log=self.shell('uname -a; getprop ro.boot.bootreason; getprop sys.boot.reason; '
                           'getprop sys.boot_completed; cat /sys/firmware/devicetree/base/chosen/bootargs',timeout=10)
            if COMMAND_LOG:(COMMAND_LOG.parent/'unexpected-android.txt').write_text(log,encoding='utf-8')

    def connect(self):
        a=self.adb_devices();f=self.fastboot_devices()
        need(len(a)+len(f)==1,'请仅连接一台手机：Android 需开启并授权 USB 调试，或手动进入 FASTBOOT。iOS 未枚举 USB 时请先关机后进 FASTBOOT')
        if a:
            self.serial,status=a[0];need(status in ('device','recovery'),'请在手机允许 USB 调试授权')
            if status=='recovery':
                self.wait_recovery_ready()
                self.check_recovery();self.prepare_android_recovery();return
            need(self.shell('getprop ro.product.device').strip()=='raphael','仅支持 Redmi K20 Pro / raphael')
            if self.shell('getprop ro.twrp.version').strip():
                self.wait_recovery_ready()
                self.check_recovery();self.prepare_android_recovery();return
            say('已识别 Android，进入 FASTBOOT。无需 Android root。')
            self.adb_command('reboot','bootloader');self.wait_fastboot()
        else:
            self.serial=f[0]
            need(self.serial!='xnuloader','当前为 iOS 诊断接口，请先正常重启回 Android 或手动进入 FASTBOOT')
        self.boot_twrp()

    def check_recovery(self):
        with Task('检查 TWRP 并释放 Android 数据分区',delay=.6):
            self._check_recovery()

    def _check_recovery(self):
        need(self.shell('getprop ro.product.device').strip()=='raphael','临时 recovery 机型不匹配')
        need('uid=0' in self.shell('id') and self.shell('getprop ro.twrp.version').strip(),'没有进入 root TWRP')
        need(int(self.shell('cat /sys/class/block/sda/queue/logical_block_size'))==BS,'仅支持 4096 字节扇区')
        # TWRP may auto-mount decrypted userdata after every temporary boot. Use
        # TWRP's unmount command, then normal unmount, and verify repeatedly.
        # Lazy unmount cannot prove that a filesystem has released the device.
        def mounts():
            for _ in range(5):
                try:
                    return {line.split()[1] for line in self.shell('cat /proc/mounts').splitlines() if len(line.split())>=2}
                except RuntimeError:
                    time.sleep(1)
            raise RuntimeError('读取 TWRP 挂载表超时')
        for _ in range(3):
            mounted=mounts()
            bad=sorted(p for p in mounted if p in ('/data','/metadata','/sdcard') or p.startswith('/data/'))
            if not bad:
                return
            for target in ('data','metadata','sdcard'):
                try:self.shell('twrp unmount '+target,60)
                except RuntimeError:pass
                time.sleep(.8)
            mounted=mounts()
            if not any(p in ('/data','/metadata','/sdcard') or p.startswith('/data/') for p in mounted):
                return
            for point in sorted(mounted,key=len,reverse=True):
                if not (point in ('/data','/metadata','/sdcard') or point.startswith(('/data/','/metadata/','/sdcard/'))):continue
                try:self.shell('umount '+shlex.quote(point),30)
                except RuntimeError:pass
            time.sleep(.8)
        mounted={line.split()[1] for line in self.shell('cat /proc/mounts').splitlines() if len(line.split())>=2}
        bad=sorted(p for p in mounted if p in ('/data','/metadata','/sdcard') or p.startswith('/data/'))
        need(not bad,'Android 数据仍挂载，停止操作：'+','.join(bad))

    def size(self,node):return int(self.shell('blockdev --getsize64 '+shlex.quote(node)).strip())

    def read(self,node,offset,n):
        need(node=='/dev/block/sda' or re.fullmatch(r'/dev/block/(?:by-name/[A-Za-z0-9_]+|bootdevice/by-name/(?:modemst1|modemst2|fsg|fsc)|sd[a-z][0-9]+|mapper/vendor(?:_[ab])?)',node),'读取目标错误')
        need(offset>=0 and offset%BS==0 and n>0 and n%BS==0,'读取范围未按块对齐')
        result=bytearray()
        # Bound each transfer; retry reads only, never replay a device write.
        for done in range(0,n,4*1024**2):
            count=min(4*1024**2,n-done);position=offset+done
            for attempt in range(2):
                value=self.shell(f'dd if={shlex.quote(node)} bs=4096 skip={position//BS} count={count//BS}',180,True)
                record_command(dict(event='block_read',node=node,offset=position,expected_bytes=count,actual_bytes=len(value),attempt=attempt+1))
                if len(value)==count:break
                need(len(value)<count,'设备读取混入额外输出：'+node+'；已停止，未采用此数据')
            need(len(value)==count,f'设备读取不完整：{node}；偏移 {position}，应读 {count} 字节，实际 {len(value)} 字节。请保留本轮命令记录，底层 dd 错误已记录。')
            result.extend(value)
        return bytes(result)

    def region_sha(self,node,offset,n):
        need(offset%BS==n%BS==0 and n>0,'哈希范围未对齐')
        result=self.shell(f'set -o pipefail || exit 1; dd if={shlex.quote(node)} bs=4096 skip={offset//BS} count={n//BS} | sha256sum',240).split()
        need(result and re.fullmatch('[0-9a-f]{64}',result[0]),'设备哈希读取失败')
        return result[0]

    def allow(self,node,start,end):
        if node in (static_guard.VENDOR,static_guard.VBMETA):
            need(self.android_profile and self.android_profile['layout']=='static' and self.session is not None and
                 (node,start,end) in getattr(self,'static_guard_ranges',set()),'没有已确认并校验的静态防覆盖块方案')
        else:need(node in (DARWIN,RECOVERY,'/dev/block/sda'),'禁止对该分区开放写入')
        self.allowed.setdefault(node,[]).append((start,end))

    def write(self,node,offset,data):
        need(len(data)>0 and offset%BS==0 and len(data)%BS==0,'写入未对齐')
        need(any(a<=offset and offset+len(data)<=b for a,b in self.allowed.get(node,[])),'写入超出已确认的范围')
        need(self.session is not None,'必须先建立本机操作记录')
        chunk=self.session/'transfer-chunk.bin';chunk.write_bytes(data)
        remote='/tmp/hackios-oneclick-chunk.bin'
        try:
            self.adb_command('push',chunk,remote,timeout=180)
            need(self.shell('sha256sum '+remote).split()[0]==digest(data),'传输到 RAM 的文件哈希错误')
            self.shell(f'dd if={remote} of={shlex.quote(node)} bs=4096 seek={offset//BS} count={len(data)//BS} conv=notrunc && sync',180)
            need(self.read(node,offset,len(data))==data,'写后完整读回不一致，已停止')
        finally:
            if chunk.exists():chunk.unlink()

def snapshot(phone):
    size=phone.size('/dev/block/sda')
    head=phone.read('/dev/block/sda',0,8*BS);tail=phone.read('/dev/block/sda',size-8*BS,8*BS)
    hs,ts=gpt_window_sizes(head[BS:2*BS],tail[-BS:],size)
    if hs>len(head):head=phone.read('/dev/block/sda',0,hs)
    if ts>len(tail):tail=phone.read('/dev/block/sda',size-ts,ts)
    result=parse_gpt(head,tail,size)
    if COMMAND_LOG:
        (COMMAND_LOG.parent/'partition-layout.json').write_text(json.dumps(dict(
            disk='/dev/block/sda',bytes=size,head_bytes=hs,tail_bytes=ts,
            parts=result['parts'],multiboot_indicators=multiboot_indicators(result)),
            ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    return result

def backup_warning(phone,record,label,error):
    record['backup_complete']=False
    record.setdefault('backup_failures',[]).append(dict(item=label,error=str(error)))
    detail=(str(error).splitlines() or ['未知错误'])[0][:240]
    warn(label+'备份未完成：'+detail+'。仅警告，继续安装；不能依赖此项备份恢复。')
    save_record(phone,record)

def optional_backup(phone,record,label,operation):
    try:
        return operation() is not False
    except (RuntimeError,OSError) as error:
        backup_warning(phone,record,label,error)
        return False

def backup_bytes(phone,record,label,path,data,key=None):
    def save():
        need(shutil.disk_usage(ROOT).free>=len(data)+512*1024**2,'剩余空间不足，跳过可选备份以保留安装空间')
        path.write_bytes(data)
        if key:record[key]=digest(data)
    return optional_backup(phone,record,label,save)

def boot_reference(phone,record,label):
    # This is a small integrity reference, independent of a full boot backup.
    try:return phone.region_sha('/dev/block/by-name/boot',0,phone.size('/dev/block/by-name/boot'))
    except (RuntimeError,OSError) as error:
        backup_warning(phone,record,label,error)
        return None

def new_session(phone,g,extract_wireless=False):
    standard_layout_only(g)
    phone.verify_android_profile()
    need(re.fullmatch('[A-Za-z0-9_.:-]+',phone.serial or ''),'设备标识格式不支持')
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S-%f')
    path=ROOT/'session-private'/phone.serial.replace(':','_')/stamp
    if shutil.disk_usage(ROOT).free<5*1024**3:
        warn('电脑剩余空间不足 5 GiB，可选备份可能不完整；仍继续，安装临时文件必须有可用空间。')
    path.mkdir(parents=True,exist_ok=False);phone.session=path
    record=dict(serial=phone.serial,disk_size=g['size'],gpt_head_sha256=digest(g['head']),gpt_tail_sha256=digest(g['tail']),
        parts=g['parts'],steps=[],completed=False,package='蜂窝上网一键包-20261104',
        android_compat=phone.android_profile,recovery_compat=phone.recovery_checks)
    record.update(backup_complete=False,backup_failures=[],backup_policy='warn_and_continue')
    save_record(phone,record)
    for key in ('head','tail'):
        backup_bytes(phone,record,'原始 GPT '+key,path/('gpt-'+key+'.original.bin'),g[key])
    optional_backup(phone,record,'userdata 头',lambda:backup_bytes(phone,record,'userdata 头',
        path/'userdata-header.before.bin',phone.read('/dev/block/by-name/userdata',0,BS)))
    with Task('备份本机启动入口与关键分区',len(SAFE_BACKUPS)) as progress:
        for index,name in enumerate(SAFE_BACKUPS):
            progress.update(index,'读取 '+name+' 并保存到电脑')
            node='/dev/block/by-name/'+name
            def capture():
                n=phone.size(node);need(n%BS==0 and 0<n<=256*1024**2,'备份分区大小异常：'+name)
                return backup_bytes(phone,record,name,path/(name+'.before.img'),phone.read(node,0,n),name+'_sha256')
            saved=optional_backup(phone,record,name,capture)
            save_record(phone,record)
            progress.update(index+1,name+(' 已保存' if saved else ' 失败，继续'))
    record['backup_complete']=not record['backup_failures']
    save_record(phone,record)
    if extract_wireless:
        with Task('自动提取本机无线资源'):
            record['wireless_inputs']=wireless.collect(phone,path,phone.serial,warn)
    save_record(phone,record)
    say('本机操作记录与成功的备份保存在：'+str(path))
    if not record['backup_complete']:warn('备份不完整；安装仍继续，缺失项不能用于恢复。')
    return record

def save_record(phone,record):
    path=phone.session/'state.json';temp=path.with_suffix('.tmp')
    temp.write_text(json.dumps(record,ensure_ascii=False,indent=2)+'\n',encoding='utf-8');os.replace(temp,path)

def journal(phone,record,stage):
    record['steps'].append(stage);save_record(phone,record);say('完成：'+stage)

def check_layout(phone,g):
    compatible(g)
    check_base_nodes(phone,g)
    need(phone.size(DARWIN)==part(g,'darwin')['blocks']*BS,'系统读到的 darwin 大小不匹配')
    need(phone.shell('readlink -f '+DARWIN).strip()=='/dev/block/sda'+str(part(g,'darwin')['index']),'darwin 节点与 GPT 不一致')
    resolved=phone.shell('readlink -f '+DARWIN).strip()
    mounts=phone.shell('cat /proc/mounts')
    need(all(line.split()[0] not in (DARWIN,resolved) for line in mounts.splitlines() if line.split()),'darwin 仍挂载，停止写入')

def check_base_nodes(phone,g):
    for name in ('recovery','misc','userdata'):
        entry=part(g,name);node='/dev/block/by-name/'+name
        need(phone.shell('readlink -f '+node).strip()=='/dev/block/sda'+str(entry['index']),name+' 节点与 GPT 不一致')
        need(phone.size(node)==entry['blocks']*BS,name+' 节点容量与 GPT 不一致')

def check_hfs(phone):
    capacity=phone.size(DARWIN);need(capacity%BS==0 and capacity//BS>=5242875,'darwin 容量不支持')
    first=phone.read(DARWIN,DATA_FIRST*BS,BS);last=phone.read(DARWIN,capacity-BS,BS)
    for b,offset in ((first,1024),(last,3072)):
        need(b[offset:offset+2]==b'HX' and struct.unpack_from('>H',b,offset+2)[0]==5,'没有找到预期的持久化 HFSX 卷；不会自动格式化')
        block,total=struct.unpack_from('>II',b,offset+40)
        need(block==BS and total==capacity//BS-DATA_FIRST,'HFSX 数据卷大小不匹配')
    return digest(first+last)

def write_stream(phone,node,offset,stream,total,label):
    done=0
    with Task(label,total,'bytes') as progress:
        while done<total:
            progress.update(done,'传输、写入并读回校验当前数据块')
            data=stream.read(min(CHUNK,total-done));need(data and len(data)%BS==0,'镜像流截断/块未对齐')
            phone.write(node,offset+done,data);done+=len(data)
            progress.update(done,'已写入并读回校验')
        need(not stream.read(1),'镜像流超出声明大小')

def write_buffer(phone,node,data,label):
    with io.BytesIO(data) as stream:write_stream(phone,node,0,stream,len(data),label)

def same_android_profile(before,after):
    if not before or not after:return False
    keys=('layout','physical','super_node','data_entry','vendor_fstabs_sha256','base_sha256')
    return all(before.get(k)==after.get(k) for k in keys)


def skipped_static_guard(error,restore=False):
    detail=(str(error).splitlines() or ['未知错误'])[0][:240]
    warn('静态 recovery 防覆盖未完成：'+detail+'。仅警告，继续；返回 Android 后 iOS 启动入口可能被还原。')
    return dict(version=1,status='skipped',patches=[],reason=str(error),restore=restore)

def prepare_static_guard(phone,restore=False):
    try:return _prepare_static_guard(phone,restore)
    except (RuntimeError,OSError,ValueError) as error:return skipped_static_guard(error,restore)

def _prepare_static_guard(phone,restore=False):
    # Dynamic ROMs return before any guard-related vendor/vbmeta access.
    if phone.android_profile['layout']!='static':
        return dict(version=1,status='dynamic_unchanged',patches=[])
    sessions=ROOT/'session-private'/phone.serial.replace(':','_')
    if sessions.exists():
        for path in sorted(sessions.glob('*/state.json'),reverse=True):
            old=json.loads(path.read_text(encoding='utf-8'))
            pending=old.get('static_recovery_guard')
            if (old.get('serial')==phone.serial and pending and pending.get('patches') and
                not old.get('static_guard_applied') and not old.get('completed') and
                same_android_profile(old.get('android_compat'),phone.android_profile)):
                static_guard.validate(pending,phone.read,phone.size)
                if (pending.get('status')=='restore')!=restore:pending=static_guard.reverse(pending)
                say('发现本机尚未完成的静态防覆盖事务，将沿用原始备份继续。')
                return pending
    with Task('只读检查静态 ROM 的 recovery 自动恢复机制'):
        evidence={}
        try:
            return static_guard.plan(phone.android_profile,phone.read,phone.size,restore,evidence=evidence)
        finally:
            if COMMAND_LOG:
                (COMMAND_LOG.parent/'static-recovery-evidence.json').write_text(
                    json.dumps(evidence,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')

def static_guard_notice(plan):
    if plan.get('status')=='skipped':return '\n本轮静态防覆盖未完成；仍继续，返回 Android 后 iOS 启动入口可能被还原。'
    if not plan.get('patches'):return ''
    if plan['status']=='restore':
        return '\n静态 ROM：同时恢复官方 recovery 恢复脚本及安装前的 vbmeta 校验标志；之后 MIUI 启动可能把 TWRP 再还原为官方 recovery。'
    return ('\n静态 ROM 额外处理：备份并停用 vendor 的官方 recovery 自动恢复脚本；'
            '将 vbmeta 的 AVB/verity 校验标志设为停用，以允许该 vendor 改动。'
            '卸载 iOS 时会恢复原脚本和原校验标志。Android boot 不修改。')

def save_static_guard(phone,record,plan):
    record['static_recovery_guard']=plan
    record['static_guard_applied']=False
    for p in plan.get('patches',[]):
        label=p['node'].rsplit('/',1)[-1]
        backup_bytes(phone,record,label+' 防覆盖原块',phone.session/(label+'.guard.before.bin'),static_guard.decode(p['before']))
        backup_bytes(phone,record,label+' 防覆盖目标块',phone.session/(label+'.guard.after.bin'),static_guard.decode(p['after']))
    save_record(phone,record)

def apply_static_guard(phone,record):
    plan=record.get('static_recovery_guard')
    if not plan or not plan.get('patches'):return
    need(phone.android_profile['layout']=='static','静态防覆盖方案不能用于动态布局')
    phone.verify_android_profile()
    try:_static_guard_preflight(phone,plan)
    except (RuntimeError,OSError) as error:
        record['static_recovery_guard']=skipped_static_guard(error,plan.get('status')=='restore')
        record['static_guard_skipped_plan']=plan
        record['static_guard_applied']=False
        save_record(phone,record)
        return
    journal(phone,record,'开始恢复静态官方恢复机制' if plan['status']=='restore' else '开始静态 recovery 防覆盖处理')
    label='恢复静态 ROM 原始脚本及校验策略' if plan['status']=='restore' else '静态 ROM 启动入口防覆盖'
    phone.static_guard_ranges={(p['node'],p['offset'],p['offset']+BS) for p in plan['patches']}
    try:
        with Task(label,len(plan['patches'])) as progress:
            static_guard.apply(plan,phone.read,phone.size,phone.allow,phone.write,
                               lambda done,total,node:progress.update(done,node.rsplit('/',1)[-1]+' 已读回校验'))
    finally:
        phone.static_guard_ranges=set()
        if hasattr(phone,'allowed'):
            phone.allowed.pop(static_guard.VENDOR,None);phone.allowed.pop(static_guard.VBMETA,None)
    record['static_guard_applied']=True
    journal(phone,record,'静态 ROM 恢复脚本及校验策略处理完成并读回校验')

def _static_guard_preflight(phone,plan):
    resolved=phone.shell('readlink -f '+static_guard.VENDOR).strip()
    for line in phone.shell('cat /proc/mounts').splitlines():
        fields=line.split()
        if len(fields)>=2 and (fields[0] in (resolved,static_guard.VENDOR) or fields[1] in ('/vendor','/mnt/vendor')):
            need(fields[1] in ('/vendor','/mnt/vendor'),'vendor 挂载位置异常，停止静态防覆盖写入')
            phone.shell('umount '+shlex.quote(fields[1]),90)
    mounts=phone.shell('cat /proc/mounts')
    need(all(line.split()[0] not in (resolved,static_guard.VENDOR) and line.split()[1] not in ('/vendor','/mnt/vendor')
             for line in mounts.splitlines() if len(line.split())>=2),
         'vendor 仍挂载，停止静态防覆盖写入')
    static_guard.validate(plan,phone.read,phone.size)

def deploy(phone,g,config,record,fresh=False,repair=False):
    stage(3,5,'检查并写入 iOS System')
    check_layout(phone,g)
    preserved=None if fresh else check_hfs(phone)
    with Task('生成本机专用启动资源'):
        resource_path,expected_resources=wireless.prepare(ROOT,phone.session,record,phone.serial,warn)
    save_record(phone,record)
    system_ok=phone.region_sha(DARWIN,0,SYSTEM_BYTES)==config['system_sha256']
    need(system_ok or fresh or repair,'System 与本包不匹配；请选择 7“重装系统”或匹配的包，更新不会自动覆盖')
    # Backup failures warn and continue; live boundaries/readback remain mandatory.
    guards={}
    for label,at in [('resource-before',RESOURCE_OFFSET-BS),('resource-after',RESOURCE_OFFSET+RESOURCE_BYTES)]:
        value=phone.read(DARWIN,at,BS);guards[label]=value
        backup_bytes(phone,record,label,phone.session/(label+'.bin'),value)
    before=None
    def capture_resources():
        nonlocal before
        before=phone.read(DARWIN,RESOURCE_OFFSET,RESOURCE_BYTES)
        backup_bytes(phone,record,'原启动资源',phone.session/'resources.before.bin',before)
    optional_backup(phone,record,'原启动资源',capture_resources)
    reference=boot_reference(phone,record,'Android boot 完整性参考')
    record.update(resource_before_sha256=digest(before) if before is not None else None,hfs_headers_before=preserved,
        boot_before_sha256=reference)
    if reference and record.get('boot_sha256'):
        need(reference==record['boot_sha256'],'Android boot 自备份后已变化，停止安装')
    save_record(phone,record)
    if not system_ok:
        if not fresh:
            def capture_system():
                need(shutil.disk_usage(ROOT).free>=SYSTEM_BYTES+512*1024**2,'空间不足，跳过可选 System 备份')
                with Task('备份原 System 到电脑',SYSTEM_BYTES,'bytes') as progress:
                    with (phone.session/'system.before.img').open('wb') as f:
                        for off in range(0,SYSTEM_BYTES,CHUNK):
                            data=phone.read(DARWIN,off,min(CHUNK,SYSTEM_BYTES-off));f.write(data)
                            progress.update(off+len(data),'已保存到电脑')
                record['system_backup_sha256']=file_sha(phone.session/'system.before.img');save_record(phone,record)
            optional_backup(phone,record,'原 System',capture_system)
        phone.allow(DARWIN,0,SYSTEM_BYTES)
        with gzip.open(ROOT/'images/system.img.gz','rb') as f:write_stream(phone,DARWIN,0,f,SYSTEM_BYTES,'System 写入')
        need(phone.region_sha(DARWIN,0,SYSTEM_BYTES)==config['system_sha256'],'System 最终哈希不匹配')
        journal(phone,record,'System 完整写入并校验')
    else:say('System 已与本包完全一致，跳过重复写入。')
    stage(4,5,'写入启动资源与 iOS 数据卷')
    if before is None or digest(before)!=expected_resources:
        phone.allow(DARWIN,RESOURCE_OFFSET,RESOURCE_OFFSET+RESOURCE_BYTES)
        with resource_path.open('rb') as f:write_stream(phone,DARWIN,RESOURCE_OFFSET,f,RESOURCE_BYTES,'启动资源写入')
        need(phone.region_sha(DARWIN,RESOURCE_OFFSET,RESOURCE_BYTES)==expected_resources,'资源最终哈希不匹配')
        journal(phone,record,'启动资源完整写入并校验')
    else:say('启动资源已一致，跳过重复写入。')
    need(phone.read(DARWIN,RESOURCE_OFFSET-BS,BS)==guards['resource-before'] and
         phone.read(DARWIN,RESOURCE_OFFSET+RESOURCE_BYTES,BS)==guards['resource-after'],'资源两侧保护块发生变化')
    if fresh:
        blocks=part(g,'darwin')['blocks']
        with Task('按所选容量准备全新 iOS 数据卷'):
            extents=list(initial_extents(ROOT/'images/data-initial.simg',blocks))
        need(all(DATA_FIRST*BS<=o and o+len(b)<=blocks*BS for o,b in extents),'HFS 初始化写入越界')
        phone.allow(DARWIN,DATA_FIRST*BS,blocks*BS)
        with Task('初始化 iOS 数据卷',sum(len(data) for _,data in extents),'bytes') as progress:
            done=0
            for off,data in extents:
                phone.write(DARWIN,off,data);done+=len(data)
                progress.update(done,'已写入并读回校验')
        check_hfs(phone);journal(phone,record,'初始化新的 iOS 数据卷')
    else:need(check_hfs(phone)==preserved,'更新过程中 HFSX 卷头发生变化')
    stage(5,5,'写入 recovery 并完成最终校验')
    apply_static_guard(phone,record)
    recovery=(ROOT/'images/ios-recovery.img').read_bytes()
    phone.allow(RECOVERY,0,len(recovery))
    if phone.read(RECOVERY,0,len(recovery))!=recovery:
        write_buffer(phone,RECOVERY,recovery,'安装 iOS 启动入口')
        journal(phone,record,'recovery 完整写入并校验')
    else:say('recovery 已一致，跳过重复写入。')
    final_boot=boot_reference(phone,record,'Android boot 最终完整性参考')
    if final_boot and reference:need(final_boot==reference,'Android boot 校验不一致')
    live=snapshot(phone);need(live['head']==g['head'] and live['tail']==g['tail'],'部署后 GPT 发生变化')
    record.update(completed=True,boot_unchanged=True if final_boot and reference else None,userdata_formatted=fresh,hfs_headers_preserved=not fresh,
        final_recovery_sha256=digest(recovery),fresh=fresh,repair_system=repair)
    journal(phone,record,'安装完成，写入内容与边界检查通过')
    if record.get('backup_failures'):warn('本轮有备份缺失，安装已继续完成；不能依赖缺失的备份恢复。详情见 state.json 的 backup_failures。')
    if record.get('static_recovery_guard',{}).get('status')=='skipped':warn('静态防覆盖未完成；返回 Android 后 iOS 启动入口可能被还原，需要重新部署 recovery 入口。')
    if record['device_resources']['mode']=='offline':
        warn('本轮已用离线兜底完成安装。Wi-Fi 与蜂窝可能不可用；修复读取条件后，可用选项 2 重新提取并更新。')
    else:ok('已装入当前手机自己的无线资源，原 NV 分区保持不变。')

def apply_partition(phone,old,new,record):
    phone.verify_android_profile()
    live=snapshot(phone);need(live['head']==old['head'] and live['tail']==old['tail'],'确认后 GPT 已变化，停止写入')
    for off,data in gpt_writes(new):phone.allow('/dev/block/sda',off,off+len(data))
    record.update(gpt_write_started=True,new_gpt_head_sha256=digest(new['head']),new_gpt_tail_sha256=digest(new['tail']))
    journal(phone,record,'开始应用已确认的 GPT 方案')
    writes=list(gpt_writes(new))
    with Task('应用本机 GPT 分区方案',len(writes)) as progress:
        for i,(off,data) in enumerate(writes):
            phone.write('/dev/block/sda',off,data);journal(phone,record,'GPT 第 '+str(i+1)+' 段写后校验通过')
            progress.update(i+1,'本段写入并读回校验通过')
    live=snapshot(phone);need(live['head']==new['head'] and live['tail']==new['tail'],'GPT 完整读回不一致')
    record['gpt_applied']=True;record['new_gpt_head_sha256']=digest(new['head']);record['new_gpt_tail_sha256']=digest(new['tail']);save_record(phone,record)
    phone.adb_command('reboot','bootloader');phone.wait_fastboot()
    need(int(re.search(r'partition-size:darwin:\s*(\S+)',phone.fb_command('getvar','partition-size:darwin'))[1],16)==part(new,'darwin')['blocks']*BS,'Bootloader 读到的 darwin 大小不匹配')
    phone.boot_twrp()
    live=snapshot(phone);need(live['head']==new['head'] and live['tail']==new['tail'],'重启后 GPT 不一致')
    check_layout(phone,new)
    format_android_data(phone,new,record,'format-result.txt')


def format_android_data(phone,g,record,log_name):
    phone.verify_android_profile()
    need(same_android_profile(record.get('android_compat'),phone.android_profile),'Android 配置与安装记录不一致，停止格式化')
    expected=phone.android_profile['data_entry'][2]
    before=android.filesystem(phone.read('/dev/block/by-name/userdata',0,BS))
    say('按原 Android 配置格式化 userdata 为 '+expected+'；Android 个人数据将被清除。')
    result=phone.shell('twrp format data',300)
    (phone.session/log_name).write_text(result,encoding='utf-8')
    need(not re.search(r'(?im)(?:^E:|\b(?:failed|failure|unable|error)\b|失败|错误)',result),
         'TWRP 报告格式化错误，已保存原始输出并停止')
    after=android.validate_formatted(phone.read('/dev/block/by-name/userdata',0,BS),expected,part(g,'userdata')['blocks']*BS,before)
    metadata=phone.read('/dev/block/by-name/metadata',0,BS)
    need(struct.unpack_from('<H',metadata,1080)[0]==0xef53,'metadata 没有有效 ext4 超级块')
    phone.check_recovery()
    record.update(android_data_formatted=True,android_format=after)
    journal(phone,record,'Android '+expected+' 格式化、UUID 更新及新边界检查通过')

def seal_gpt_header(block,table):
    """Return one GPT header with the supplied table CRC and a new header CRC."""
    b=bytearray(block);size=struct.unpack_from('<I',b,12)[0]
    need(92<=size<=BS,'GPT 头长度不支持')
    struct.pack_into('<I',b,88,zlib.crc32(table)&0xffffffff)
    struct.pack_into('<I',b,16,0)
    struct.pack_into('<I',b,16,zlib.crc32(bytes(b[:size]))&0xffffffff)
    return bytes(b)

def removal_plan(g):
    """Build a checked GPT that removes only darwin and restores userdata to the old free end."""
    compatible(g)
    u=part(g,'userdata');d=part(g,'darwin')
    need(d['first']==u['last']+1 and d['last']<=g['ph']['last_usable'],
         'darwin 不紧邻 userdata，拒绝自动删除')
    table=bytearray(g['table']);ui=(u['index']-1)*128;di=(d['index']-1)*128
    struct.pack_into('<Q',table,ui+40,d['last'])
    table[di:di+128]=bytes(128)
    head=bytearray(g['head']);at=(g['ph']['entries_lba'])*BS
    head[at:at+len(table)]=table
    head[BS:2*BS]=seal_gpt_header(head[BS:2*BS],bytes(table))
    tail=bytearray(g['tail']);at=(g['bh']['entries_lba']-(g['blocks']-len(g['tail'])//BS))*BS
    tail[at:at+len(table)]=table
    tail[-BS:]=seal_gpt_header(tail[-BS:],bytes(table))
    new=parse_gpt(bytes(head),bytes(tail),g['size'])
    modified=[i for i in range(g['ph']['count'])
              if g['table'][i*128:(i+1)*128]!=new['table'][i*128:(i+1)*128]]
    need(modified==sorted([u['index']-1,d['index']-1]),'删除方案修改了非 userdata/darwin 条目')
    nu=part(new,'userdata');need(nu['first']==u['first'] and nu['last']==d['last'],'userdata 恢复边界不正确')
    need(not any(p['name']=='darwin' for p in new['parts']),'删除方案仍包含 darwin')
    need(g['head'][:BS]==new['head'][:BS],'删除方案修改了保护 MBR')
    return new

def remove_ios(phone,config):
    stage(1,4,'连接手机并检查 darwin 布局')
    phone.connect();g=snapshot(phone)
    need(any(p['name']=='darwin' for p in g['parts']),'没有 darwin 分区，无需移除 iOS；未格式化或写入。')
    compatible(g);check_base_nodes(phone,g);battery_check(phone)
    resolved=phone.shell('readlink -f '+DARWIN).strip()
    need(resolved=='/dev/block/sda'+str(part(g,'darwin')['index']),'darwin 节点与 GPT 不一致')
    mounts=phone.shell('cat /proc/mounts')
    need(all(line.split()[0] not in (DARWIN,resolved) for line in mounts.splitlines() if line.split()),'darwin 仍挂载，停止删除')
    new=removal_plan(g)
    guard_plan=prepare_static_guard(phone,restore=True)
    confirm('将删除 darwin 分区、恢复 userdata 原始边界、格式化 Android userdata/metadata，并把 recovery 恢复为包内 TWRP。iOS 数据将不可恢复。'+static_guard_notice(guard_plan),'REMOVE-IOS')
    record=new_session(phone,g)
    save_static_guard(phone,record,guard_plan)
    record.update(removing_ios=True,new_gpt_head_sha256=digest(new['head']),new_gpt_tail_sha256=digest(new['tail']))
    save_record(phone,record)
    phone.verify_android_profile()
    live=snapshot(phone);need(live['head']==g['head'] and live['tail']==g['tail'],'确认后 GPT 已变化，停止写入')
    apply_static_guard(phone,record)
    for off,data in gpt_writes(new):phone.allow('/dev/block/sda',off,off+len(data))
    stage(2,4,'删除 darwin 并恢复 userdata 边界')
    writes=list(gpt_writes(new))
    with Task('移除 darwin 并恢复 Android 容量',len(writes)) as progress:
        for i,(off,data) in enumerate(writes):
            phone.write('/dev/block/sda',off,data);journal(phone,record,'删除 iOS GPT 第 '+str(i+1)+' 段写后校验通过')
            progress.update(i+1,'本段写入并读回校验通过')
    need(snapshot(phone)['head']==new['head'] and snapshot(phone)['tail']==new['tail'],'删除后 GPT 读回不一致')
    record['gpt_removed']=True;save_record(phone,record)
    phone.adb_command('reboot','bootloader');phone.wait_fastboot();phone.boot_twrp()
    live=snapshot(phone);compatible(live,False);check_base_nodes(phone,live)
    need(live['head']==new['head'] and live['tail']==new['tail'],'重启后 GPT 与移除方案不一致')
    need(not any(p['name']=='darwin' for p in live['parts']),'重启后仍检测到 darwin')
    need(part(live,'userdata')['last']==part(new,'userdata')['last'],'重启后 userdata 未恢复到移除方案的边界')
    stage(3,4,'格式化 Android userdata 和 metadata')
    format_android_data(phone,live,record,'remove-format-result.txt')
    stage(4,4,'恢复 TWRP recovery 并完成校验')
    image=phone.recovery_image.read_bytes();phone.allow(RECOVERY,0,len(image))
    if phone.read(RECOVERY,0,len(image))!=image:
        write_buffer(phone,RECOVERY,image,'恢复 TWRP 启动入口')
        journal(phone,record,'恢复匹配当前 Android 的 TWRP recovery 并完整读回校验')
    else:say('recovery 已是包内 TWRP，跳过重复写入。')
    record['completed']=True;journal(phone,record,'iOS 环境已移除；GPT、userdata、metadata、recovery 校验通过')
    say('清理完成。手机停留在临时 TWRP；可输入 Y 重启进入 Android。')
    if input('输入 Y 重启 Android，其他输入保持 TWRP：').strip().upper()=='Y':phone.adb_command('reboot')

def battery_check(phone):
    value=phone.shell('cat /sys/class/power_supply/battery/capacity').strip()
    need(value.isdigit() and int(value)>=25,'请先回 Android 将电量充至 25% 以上，再进行安装')

def finish(phone):
    say('\n安装完成。正常开机进入 Android；关机后电源＋音量上进入 iOS。')
    choice=input('现在进入 iOS？输入 Y；其他输入停留在临时 TWRP：').strip().upper()
    if choice=='Y':
        phone.adb_command('reboot','recovery')
        say('已请求 recovery 启动，请等待手机进入 iOS。程序到此结束，不会定时重启。')

def install(phone,config,fresh=False,repair=False):
    section('连接与设备检查'); stage(1,5,'连接手机并校验机型、Bootloader 和 GPT')
    phone.connect();g=snapshot(phone);battery_check(phone)
    ok('设备连接与电量检查通过')
    if g.get('parts') is not None:standard_layout_only(g)
    if not fresh and g.get('parts') is not None:
        need(any(p['name']=='darwin' for p in g['parts']),
             '当前没有 iOS 的 darwin 分区，不能更新/重装。首次安装请选择 1（会清除 Android 数据）；首次安装尚未创建分区就中断时也重新选 1，已经创建则选 8 并保留原记录。')
    guard_plan=prepare_static_guard(phone)
    if fresh:
        need(not any(p['name']=='darwin' for p in g['parts']),'已有 darwin，禁止重复首次分区。请选择保留数据更新；若前次首次安装中断，请使用“续装”')
        # Plan files live in the local session. Backups precede any write or destructive confirmation.
        compatible(g,False);check_base_nodes(phone,g)
        lo,hi=capacity_limits(g);need(hi>=lo,'剩余空间不足，无法同时保留 Android 与 iOS')
        if part(g,'userdata')['last']!=g['ph']['last_usable']:
            warn('userdata 不在磁盘末尾：仅从 userdata 自身尾部划分空间，其后的分区和空闲区域保持原样。')
        section('自行设置 iOS 容量')
        say(f'可选 {lo}～{hi} GiB；至少为 Android 保留 8 GiB。没有默认容量。')
        say(f'iOS 分区内约 {DATA_FIRST*BS/1024**3:.2f} GiB 为系统和预留区域，其余用于 iOS 数据。')
        while True:
            value=input('输入 iOS 分区容量（整数 GiB；输入 Q 取消）：').strip()
            if value.upper()=='Q':raise RuntimeError('操作已取消')
            if not re.fullmatch(r'[0-9]{1,4}',value) or not lo<=int(value)<=hi:
                warn(f'请输入 {lo}～{hi} 的整数；不会使用默认值。');continue
            gib=validate_capacity(g,int(value));break
        record=new_session(phone,g,extract_wireless=True);new=make_plan(g,phone.session/'partition-plan',ROOT/'tools/userdata-carve.py',gib)
        record.update(requested_ios_gib=gib,darwin_blocks=part(new,'darwin')['blocks']);save_record(phone,record)
        save_static_guard(phone,record,guard_plan)
        old_ud=part(g,'userdata');new_ud=part(new,'userdata');d=part(new,'darwin')
        section('首次安装方案'); say(f'Android userdata：{old_ud["blocks"]*BS/1024**3:.2f} GiB → {new_ud["blocks"]*BS/1024**3:.2f} GiB'); say(f'新增 darwin：{d["blocks"]*BS/1024**3:.3f} GiB（按 2 MiB 对齐）'); say(f'iOS 数据卷：{(d["blocks"]-DATA_FIRST)*BS/1024**3:.2f} GiB'); say('仅修改 userdata 末端并新增 darwin，其余 GPT 条目保持不变。')
        confirm('此操作会清除 Android 应用、照片、文件、账号和加密密钥。'+static_guard_notice(guard_plan))
        record['fresh_authorized']=True;save_record(phone,record)
        stage(2,5,'写入并校验 GPT 分区方案')
        apply_partition(phone,g,new,record);deploy(phone,new,config,record,fresh=True)
    else:
        check_layout(phone,g);check_hfs(phone)
        confirm('将更新 darwin 启动资源和 recovery，保留 Android 与 iOS 数据。'+('本操作还会检查并按需重写 System，写入前备份原 System。' if repair else '')+static_guard_notice(guard_plan))
        record=new_session(phone,g,extract_wireless=True);save_static_guard(phone,record,guard_plan);deploy(phone,g,config,record,repair=repair)
    finish(phone)

def validate_resume_backups(phone,g,record):
    need(record.get('serial')==phone.serial and record.get('disk_size')==g['size'],'续装记录设备或磁盘容量不匹配')
    if record.get('backup_complete') is False:
        warn('原会话备份不完整；仅警告，仍按匹配的设备及已授权 GPT 方案续装。')
    def original_gpt():
        blobs=[]
        for filename,key in (('gpt-head.original.bin','gpt_head_sha256'),('gpt-tail.original.bin','gpt_tail_sha256')):
            path=phone.session/filename
            need(path.is_file() and file_sha(path)==record.get(key),'原始 GPT 备份缺失或损坏：'+filename)
            blobs.append(path.read_bytes())
        parse_gpt(*blobs,g['size'])
    optional_backup(phone,record,'续装原始 GPT',original_gpt)
    for name in SAFE_BACKUPS:
        def verify():
            path=phone.session/(name+'.before.img');expected=record.get(name+'_sha256')
            need(path.is_file() and path.stat().st_size==phone.size('/dev/block/by-name/'+name) and
                 file_sha(path)==expected,'备份缺失、长度变化或损坏：'+name)
        optional_backup(phone,record,'续装 '+name,verify)
    reference=boot_reference(phone,record,'续装 boot 完整性参考')
    if reference and record.get('boot_sha256'):
        need(reference==record['boot_sha256'],'Android boot 自原会话后已变化，禁止格式化或续装')

def resume_fresh(phone,config):
    phone.connect();g=snapshot(phone)
    need(any(p['name']=='darwin' for p in g['parts']),
         '尚未找到 darwin，说明分区尚未创建或已移除。请保留日志；确认未发生分区写入后使用选项 1，不能使用续装。')
    check_layout(phone,g);battery_check(phone)
    sessions=ROOT/'session-private'/phone.serial.replace(':','_')
    candidates=[]
    if sessions.exists():
        for state in sorted(sessions.glob('*/state.json'),reverse=True):
            value=json.loads(state.read_text(encoding='utf-8'))
            if (value.get('serial')==phone.serial and value.get('fresh_authorized') and
                (value.get('gpt_applied') or value.get('gpt_write_started')) and not value.get('completed') and
                value.get('new_gpt_head_sha256')==digest(g['head']) and value.get('new_gpt_tail_sha256')==digest(g['tail'])):
                candidates.append((state,value))
    need(candidates,'没有找到本包创建且中断的首次安装记录；不能把他人的现有 darwin 当成空数据卷')
    state,record=candidates[0];phone.session=state.parent
    need(same_android_profile(record.get('android_compat'),phone.android_profile),'旧记录缺少兼容性信息或 Android 配置已变化，禁止盲目续装')
    validate_resume_backups(phone,g,record)
    guard_plan=record.get('static_recovery_guard') or prepare_static_guard(phone)
    if guard_plan.get('patches'):
        try:static_guard.validate(guard_plan,phone.read,phone.size)
        except (RuntimeError,OSError) as error:guard_plan=skipped_static_guard(error)
    confirm('续装本包之前已授权的首次安装。将重新初始化 iOS 数据卷；如果 Android 格式化还未完成，也会重新格式化 Android 数据。'+static_guard_notice(guard_plan),'RESUME')
    save_static_guard(phone,record,guard_plan)
    if not record.get('android_data_formatted'):
        format_android_data(phone,g,record,'resume-format-result.txt')
    else:
        android.validate_formatted(phone.read('/dev/block/by-name/userdata',0,BS),phone.android_profile['data_entry'][2],part(g,'userdata')['blocks']*BS)
    deploy(phone,g,config,record,fresh=True);finish(phone)

def restore_recovery(phone,config,original=False):
    phone.connect();g=snapshot(phone);compatible(g,False);check_base_nodes(phone,g);battery_check(phone)
    if original:
        image=phone.recovery_image
        confirm('将 recovery 替换为匹配当前 Android 分区与 Data 格式的 TWRP。按键启动 iOS 的入口将被替换，darwin 中的数据保留。','TWRP')
    else:
        sessions=ROOT/'session-private'/phone.serial.replace(':','_')
        choices=sorted(sessions.glob('*/recovery.before.img'),reverse=True) if sessions.exists() else []
        need(choices,'未找到此手机的自动备份')
        say('可恢复的 recovery 备份：')
        for i,p in enumerate(choices):say(str(i+1)+'. '+p.parent.name)
        selected=input('选择备份序号：').strip();need(selected.isdigit() and 1<=int(selected)<=len(choices),'无效序号')
        image=choices[int(selected)-1]
        state=json.loads((image.parent/'state.json').read_text(encoding='utf-8'))
        need(state['serial']==phone.serial and file_sha(image)==state['recovery_sha256'],'本机备份哈希/设备不匹配')
        confirm('将只恢复选中的 recovery。不会恢复 GPT 或已删除的 Android 数据。','RESTORE')
    data=image.read_bytes();need(len(data)==64*1024**2 and data[:8]==b'ANDROID!','recovery 备份格式不正确')
    record=new_session(phone,g);phone.allow(RECOVERY,0,len(data))
    write_buffer(phone,RECOVERY,data,'恢复选定的启动入口')
    record['completed']=True;journal(phone,record,'恢复 recovery 并完整读回校验')
    say('恢复完成，停留在临时 TWRP。')

def inspect(phone):
    phone.connect();g=snapshot(phone);compatible(g,False);check_base_nodes(phone,g)
    say('机型 raphael，GPT 主副 CRC 与边界检查通过。')
    has=any(p['name']=='darwin' for p in g['parts'])
    if has:
        check_layout(phone,g);check_hfs(phone);say('存在匹配的 darwin/HFSX，可使用保留数据更新。')
    else:
        lo,hi=capacity_limits(g);say(f'没有 darwin；首次安装需自行输入 {lo}～{hi} GiB，并清除 Android 数据。')
    guard_plan=prepare_static_guard(phone)
    if guard_plan.get('patches'):
        say('静态 recovery 防覆盖：'+('已安装并通过只读校验。' if guard_plan['status']=='present' else '需要安装；保留数据更新会处理官方自动恢复机制。'))
    say('检查模式没有写入分区；当前停留在临时 TWRP。')

def main():
    _enable_colors()
    ap=argparse.ArgumentParser();ap.add_argument('--verify',action='store_true');ap.add_argument('--update',action='store_true');args=ap.parse_args()
    say('\n  H A C K i O S',UI.bold+UI.cyan)
    say('  红米 K20 Pro  /  蜂窝上网版·数据卸载修正版  /  2026.11.04',UI.bold)
    say('  酷安：你说喜欢是装的  ',UI.bold+'\033[97;44m')
    say('\n  适用机型 raphael · 操作确认 Y / N',UI.dim)
    config=verify_package()
    if args.verify:return
    if args.update:
        install(Phone(),config,repair=True)
        return
    section('安装与更新')
    say('  1  首次安装    新增 darwin；清除 Android 用户数据',UI.yellow)
    say('  2  更新已有 iOS 必须已有 darwin；保留两套数据',UI.green)
    say('  8  继续安装    仅用于本包首次安装中断，需原备份记录')
    section('检查与维护')
    say('  3  检查手机    只读检查分区，不写入')
    say('  4  临时 TWRP   临时启动，不替换启动入口')
    say('  5  恢复 TWRP   替换 recovery 入口，保留 iOS 数据')
    say('  6  恢复备份    仅恢复本机备份的 recovery 启动入口')
    say('  7  重装系统    重写 iOS System，保留 iOS 数据卷')
    say('\n  9  移除 iOS    恢复 Android 容量；清除两套系统用户数据',UI.red)
    say('  10 连接爱思    iOS 桌面手动开启；无 3 分钟上限',UI.cyan)
    say('  0  退出\n',UI.dim)
    action=input('\n选择序号：').strip()
    if action=='0':return
    phone=Phone()
    if action=='1':install(phone,config,fresh=True)
    elif action=='2':install(phone,config,repair=True)
    elif action=='3':inspect(phone)
    elif action=='4':phone.connect();say('已进入临时 TWRP，未刷写 recovery。')
    elif action=='5':restore_recovery(phone,config,original=True)
    elif action=='6':restore_recovery(phone,config)
    elif action=='7':install(phone,config,repair=True)
    elif action=='8':resume_fresh(phone,config)
    elif action=='9':remove_ios(phone,config)
    elif action=='10':
        from computer_link import main as computer_link_main
        try:computer_link_main(verify=False)
        except (Exception,KeyboardInterrupt):raise RuntimeError('爱思连接未完成，请按使用说明核查接口和驱动；未刷写或自动重启。') from None
    else:raise RuntimeError('无效选项')

if __name__=='__main__':
    try:main()
    except (Exception,KeyboardInterrupt) as e:
        say('\n已停止：'+str(e))
        if ACTIVE_PHONE:
            try:ACTIVE_PHONE.preserve_failure_log()
            except (Exception,KeyboardInterrupt):pass
        say('不会自动重启或继续刷写。保留 session-private 里的备份与 state.json；请按使用说明处理。')
        sys.exit(1)

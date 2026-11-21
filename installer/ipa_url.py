"""Serve only a user-selected IPA for TrollStore Lite URL installation."""
from pathlib import Path
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
import argparse,contextlib,hashlib,ipaddress,json,os,re,subprocess,sys,tempfile,threading,time,zipfile
from urllib.parse import quote
sys.path.insert(0,str(Path(__file__).resolve().parent))
from console_ui import console,Task

def choose_ipa():
    """Use Windows' Unicode file picker; the embedded runtime has no tkinter."""
    if os.name!='nt':raise OSError('当前系统不支持 Windows 文件选择器，请粘贴文件路径。')
    import ctypes
    from ctypes import wintypes as w
    class OPENFILENAMEW(ctypes.Structure):
        _fields_=[('lStructSize',w.DWORD),('hwndOwner',w.HWND),('hInstance',w.HINSTANCE),
            ('lpstrFilter',w.LPCWSTR),('lpstrCustomFilter',w.LPWSTR),('nMaxCustFilter',w.DWORD),
            ('nFilterIndex',w.DWORD),('lpstrFile',w.LPWSTR),('nMaxFile',w.DWORD),
            ('lpstrFileTitle',w.LPWSTR),('nMaxFileTitle',w.DWORD),('lpstrInitialDir',w.LPCWSTR),
            ('lpstrTitle',w.LPCWSTR),('Flags',w.DWORD),('nFileOffset',w.WORD),
            ('nFileExtension',w.WORD),('lpstrDefExt',w.LPCWSTR),('lCustData',ctypes.c_ssize_t),
            ('lpfnHook',ctypes.c_void_p),('lpTemplateName',w.LPCWSTR),('pvReserved',ctypes.c_void_p),
            ('dwReserved',w.DWORD),('FlagsEx',w.DWORD)]
    dialog=ctypes.WinDLL('comdlg32',use_last_error=True)
    get_file=dialog.GetOpenFileNameW;get_file.argtypes=[ctypes.POINTER(OPENFILENAMEW)];get_file.restype=w.BOOL
    get_error=dialog.CommDlgExtendedError;get_error.argtypes=[];get_error.restype=w.DWORD
    kernel=ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetConsoleWindow.argtypes=[];kernel.GetConsoleWindow.restype=w.HWND
    buffer=ctypes.create_unicode_buffer(32768);request=OPENFILENAMEW()
    request.lStructSize=ctypes.sizeof(request);request.hwndOwner=kernel.GetConsoleWindow()
    request.lpstrFilter='IPA 应用文件 (*.ipa)\0*.ipa\0所有文件 (*.*)\0*.*\0\0'
    request.nFilterIndex=1;request.lpstrFile=ctypes.cast(buffer,w.LPWSTR);request.nMaxFile=len(buffer)
    request.lpstrTitle='选择电脑上已下载的 IPA 应用文件'
    request.lpstrInitialDir=str(Path(__file__).resolve().parent/'应用安装')
    request.lpstrDefExt='ipa';request.Flags=0x80000|0x1000|0x800|0x8|0x2000000|0x4
    if get_file(ctypes.byref(request)):return buffer.value
    error=get_error()
    if error:raise OSError(f'文件选择器打开失败（0x{error:08x}），请粘贴文件路径。')
    return None

def select_ipa(argument=None,manual=False):
    raw=argument
    while True:
        if raw is None and not manual:
            console.write('请选择电脑上的 IPA 文件；无需拖进命令窗口。')
            try:raw=choose_ipa()
            except OSError as e:console.write(str(e));manual=True
            else:
                if raw is None:return None
        if raw is None:
            raw=input('粘贴 IPA 完整路径后回车；直接回车打开文件选择器，Q 退出：').strip()
            if not raw:manual=False;raw=None;continue
        raw=str(raw).strip().strip('"')
        if raw.upper()=='Q':return None
        try:return validate_ipa(raw)
        except (OSError,ValueError,zipfile.BadZipFile) as e:
            console.write('文件不可用：'+str(e));raw=None;manual=True

def validate_ipa(path):
    path=Path(path).expanduser().resolve()
    if not path.is_file() or path.suffix.lower()!='.ipa':raise ValueError('请选择电脑上已下载好的 .ipa 文件。')
    console.write('正在检查 IPA 应用结构：'+path.name)
    with Task('检查 IPA 应用结构'),zipfile.ZipFile(path) as archive:
        if not any(re.fullmatch(r'Payload/[^/]+\.app/Info\.plist',n) for n in archive.namelist()):
            raise ValueError('该文件没有标准 IPA 的 Payload 应用结构，可能下载成了网页或压缩包。')
    return path

def lan_ipv4(value):
    try:address=ipaddress.IPv4Address(value)
    except (ipaddress.AddressValueError,TypeError):return False
    return not (address.is_loopback or address.is_link_local or address.is_multicast or
                address.is_unspecified or address.is_reserved or int(address)==0xffffffff)

def network_interfaces():
    """Read actual adapter type instead of using a default route that may belong to a VPN."""
    if os.name!='nt':raise OSError('自动筛选网卡仅支持 Windows，请输入电脑的局域网 IPv4 地址。')
    command=r'''$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new()
@(Get-NetIPConfiguration | ForEach-Object {
 $c=$_; $a=$c.NetAdapter
 foreach($v in $c.IPv4Address) {
  [pscustomobject]@{address=$v.IPAddress;prefix=$v.PrefixLength;name=$c.InterfaceAlias;
   index=$c.InterfaceIndex;up=($a.Status -eq 'Up');physical=[bool]$a.HardwareInterface;
   gateway=[bool]$c.IPv4DefaultGateway}
 }
}) | ConvertTo-Json -Compress -Depth 3
'''
    executable=Path(os.environ.get('SystemRoot','C:/Windows'))/'System32/WindowsPowerShell/v1.0/powershell.exe'
    result=subprocess.run([str(executable),'-NoProfile','-NonInteractive','-Command',command],
        capture_output=True,encoding='utf-8',errors='replace',timeout=20,
        creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
    if result.returncode:raise OSError('无法读取网卡信息，请输入电脑的局域网 IPv4 地址。')
    try:
        rows=json.loads(result.stdout.lstrip('\ufeff') or '[]')
        if isinstance(rows,dict):rows=[rows]
        if not isinstance(rows,list):raise ValueError('network list')
        return rows
    except (ValueError,TypeError) as e:raise OSError('网卡信息格式异常，请手动输入地址。') from e

def lan_candidates(rows):
    candidates={}
    for row in rows:
        address=row.get('address')
        if row.get('up') is not True or row.get('physical') is not True or not lan_ipv4(address):continue
        if address not in candidates:candidates[address]=row
    return sorted(candidates.values(),key=lambda r:(not bool(r.get('gateway')),r.get('index',0),r['address']))

def select_address(host=None):
    try:
        with Task('检测电脑已连接的网卡'):rows=network_interfaces()
    except (OSError,subprocess.TimeoutExpired) as e:
        console.write('自动筛选未完成：'+('读取网卡超时。' if isinstance(e,subprocess.TimeoutExpired) else str(e)))
        rows=None
    if host:
        if not lan_ipv4(host):raise ValueError('请输入有效的局域网 IPv4 地址。')
        if rows is not None and not any(r.get('address')==host and r.get('up') is True for r in rows):
            raise ValueError('指定地址不属于电脑当前已连接的网卡。')
        return host
    candidates=lan_candidates(rows or [])
    if len(candidates)==1:
        row=candidates[0];console.write('已自动选择：'+row['name']+' · '+row['address'])
        return row['address']
    if candidates:
        console.write('发现多个已连接的物理网卡，请选择与手机相通的那一个：')
        for i,row in enumerate(candidates,1):
            console.write(f"  {i}  {row['name']} · {row['address']}/{row['prefix']}"+
                  ('（有默认网关）' if row.get('gateway') else ''))
    else:console.write('未找到可自动推荐的物理网卡，请先确认电脑已联网。')
    while True:
        value=input('输入网卡序号（回车选 1），或粘贴电脑 IPv4，Q 退出：' if candidates else
                    '粘贴电脑的局域网 IPv4 地址，Q 退出：').strip()
        if value.upper()=='Q':return None
        if candidates and not value:return candidates[0]['address']
        if candidates and value.isdigit() and 1<=int(value)<=len(candidates):return candidates[int(value)-1]['address']
        if lan_ipv4(value) and (rows is None or any(r.get('address')==value and r.get('up') is True for r in rows)):
            return value
        console.write('地址或序号无效；应填写电脑当前使用的 IPv4，不是手机地址或路由器地址。')

def default_index_path():
    parent=Path(os.environ.get('LOCALAPPDATA') or Path.home()/'AppData/Local')
    return parent/'HackiOS'/'ipa-links.json'

@contextlib.contextmanager
def index_lock(path):
    """Use an OS lock: concurrent tools share IDs and a crashed process releases it."""
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.with_suffix('.lock').open('a+b') as lock:
        lock.seek(0,2)
        if lock.tell()==0:lock.write(b'\0');lock.flush()
        if os.name=='nt':
            import msvcrt
            def acquire():lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_NBLCK,1)
            def release():lock.seek(0);msvcrt.locking(lock.fileno(),msvcrt.LK_UNLCK,1)
        else:
            import fcntl
            def acquire():fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
            def release():fcntl.flock(lock.fileno(),fcntl.LOCK_UN)
        until=time.monotonic()+10
        while True:
            try:acquire();break
            except OSError:
                if time.monotonic()>=until:raise OSError('编号记录正被其他窗口占用，请稍后再试。') from None
                time.sleep(.05)
        try:yield
        finally:release()

def file_number(fingerprint,index_path=None):
    path=Path(index_path) if index_path is not None else default_index_path()
    with index_lock(path):
        try:
            state=json.loads(path.read_text(encoding='utf-8')) if path.exists() else dict(version=1,next_id=2,files={})
            files=state['files'];next_id=state['next_id']
            if (state['version']!=1 or not isinstance(files,dict) or type(next_id) is not int or next_id<2 or
                any(not re.fullmatch('[0-9a-f]{64}',key) or type(value) is not int or value<2 for key,value in files.items()) or
                len(set(files.values()))!=len(files) or next_id<=max(files.values(),default=1)):
                raise ValueError('invalid index')
        except (ValueError,KeyError,TypeError,AttributeError):
            raise ValueError('应用编号记录损坏，请保留 '+str(path)+' 后排查；不会重置编号或复用旧链接。') from None
        if fingerprint in files:return files[fingerprint]
        files[fingerprint]=next_id;state['next_id']=next_id+1
        temporary=None
        try:
            with tempfile.NamedTemporaryFile('w',encoding='utf-8',dir=path.parent,prefix='ipa-links-',suffix='.tmp',delete=False) as stream:
                temporary=Path(stream.name);json.dump(state,stream,ensure_ascii=False,indent=2)
                stream.write('\n');stream.flush();os.fsync(stream.fileno())
            os.replace(temporary,path)
        finally:
            if temporary is not None:temporary.unlink(missing_ok=True)
        return next_id

def handler_for(path,index_path=None,progress=False):
    path=Path(path).resolve()
    before=path.stat()
    digest=hashlib.sha256();done=0
    if progress:console.write('正在读取并校验：'+path.name+'；完成后显示短数字链接。')
    task=Task('读取并校验 IPA',before.st_size,unit='bytes') if progress else contextlib.nullcontext()
    with task as meter,path.open('rb') as stream:
        while data:=stream.read(4*1024**2):
            digest.update(data);done+=len(data)
            if meter is not None:meter.update(done,path.name)
    fingerprint=digest.hexdigest()
    selected_stat=path.stat();identity=(selected_stat.st_size,selected_stat.st_mtime_ns)
    if identity!=(before.st_size,before.st_mtime_ns):raise ValueError('计算链接时 IPA 已发生变化，请重新选择。')
    number=file_number(fingerprint,index_path);route='/'+str(number)
    class Handler(BaseHTTPRequestHandler):
        download_path=route
        download_sha256=fingerprint
        download_number=number
        download_name=path.name
        def log_message(self,*args):pass
        def end_headers(self):
            self.send_header('Cache-Control','no-store, no-cache, must-revalidate')
            self.send_header('Pragma','no-cache');self.send_header('Expires','0')
            super().end_headers()
        def reply(self,head=False):
            if self.path!=route:
                self.send_error(404,'Link expired or unknown; use the URL currently shown on the computer');return
            start_transfer=getattr(self.server,'begin_transfer',None)
            if start_transfer is not None and not start_transfer(type(self)):
                self.send_error(503,'Choosing another file; retry the URL shown on the computer');return
            try:self.send_file(head)
            finally:
                if start_transfer is not None:self.server.end_transfer()
        def send_file(self,head):
            self.connection.settimeout(30)
            try:current=path.stat()
            except OSError:
                self.send_error(410,'Selected file is no longer available');return
            if (current.st_size,current.st_mtime_ns)!=identity:
                self.send_error(409,'Selected file changed; restart the helper and select it again');return
            size=identity[0];start,end=0,size-1;partial=False
            value=self.headers.get('Range')
            if value:
                m=re.fullmatch(r'bytes=(\d+)-(\d*)',value.strip())
                if not m:self.send_error(416);return
                start=int(m[1]);end=min(int(m[2]) if m[2] else size-1,size-1);partial=True
                if start>end or start>=size:self.send_error(416);return
            self.send_response(206 if partial else 200)
            self.send_header('Content-Type','application/octet-stream')
            self.send_header('Content-Disposition',f'attachment; filename="{number}.ipa"; filename*=UTF-8\'\''+quote(path.name,safe=''))
            self.send_header('X-Content-SHA256',fingerprint)
            self.send_header('Accept-Ranges','bytes');self.send_header('Content-Length',str(end-start+1))
            if partial:self.send_header('Content-Range',f'bytes {start}-{end}/{size}')
            self.end_headers()
            if head:return
            try:
                label=f'向手机传输 #{number} · '+path.name
                task=Task(label,end-start+1,unit='bytes') if progress else contextlib.nullcontext()
                with task as meter,path.open('rb') as f:
                    f.seek(start);left=end-start+1;done=0
                    while left:
                        b=f.read(min(left,1024*1024))
                        if not b:raise OSError('IPA 文件在传输中提前结束，请重新选择。')
                        self.wfile.write(b);left-=len(b);done+=len(b)
                        if meter is not None:meter.update(done,path.name)
                if progress:console.write(f'#{number} 本次传输完成（'+('部分文件' if partial else '完整文件')+'）；请在巨魔中等待安装结果。')
            except (BrokenPipeError,ConnectionResetError,ConnectionAbortedError,TimeoutError):
                if progress:console.write(f'#{number} 本次传输中断；请保持手机联网，按巨魔提示重试。')
        def do_GET(self):self.reply()
        def do_HEAD(self):self.reply(True)
    return Handler

class IPAServer(ThreadingHTTPServer):
    def __init__(self,*args,**kwargs):
        self.selection_lock=threading.Lock();self.selecting=False;self.transfers=0
        super().__init__(*args,**kwargs)
    def begin_transfer(self,handler):
        with self.selection_lock:
            if self.selecting or self.RequestHandlerClass is not handler:return False
            self.transfers+=1;return True
    def end_transfer(self):
        with self.selection_lock:self.transfers-=1
    def pause_selection(self):
        with self.selection_lock:
            if self.transfers:return False
            self.selecting=True;return True
    def finish_selection(self,handler=None):
        with self.selection_lock:
            if handler is not None:self.RequestHandlerClass=handler
            self.selecting=False

def show_selection(server):
    handler=server.RequestHandlerClass;address,port=server.server_address[:2]
    console.write('\n当前应用：#'+str(handler.download_number)+' → '+handler.download_name)
    console.write(f'  http://{address}:{port}{handler.download_path}')
    console.write('在巨魔选择“通过 URL 安装”。换 App 只需替换末尾数字，仍以本次完整地址为准。')
    console.write('保持窗口打开。回车／N 选择下一个 IPA；Q 或 Ctrl+C 退出。\n')

def wait_action():
    """Single keys avoid a blocking input prompt being overwritten by live progress."""
    if os.name=='nt' and sys.stdin.isatty():
        import msvcrt
        while True:
            if msvcrt.kbhit():
                key=msvcrt.getwch()
                if key in ('\0','\xe0'):msvcrt.getwch();continue
                if key=='\x03':raise KeyboardInterrupt
                if key in ('\r','\n','n','N'):return 'next'
                if key in ('q','Q'):return 'quit'
            time.sleep(.08)
    elif sys.stdin.isatty():
        return 'quit' if input('回车换文件，Q 退出：').strip().upper()=='Q' else 'next'
    else:
        while True:time.sleep(.2)

def main():
    console.enable()
    parser=argparse.ArgumentParser(description='将电脑选定的 IPA 提供为巨魔可直接下载的局域网 URL。')
    parser.add_argument('ipa',nargs='?');parser.add_argument('--port',type=int,default=8876)
    parser.add_argument('--manual',action='store_true',help='手动粘贴 IPA 文件路径')
    parser.add_argument('--host',help='手动指定电脑的局域网 IPv4 地址')
    args=parser.parse_args();path=select_ipa(args.ipa,args.manual)
    if path is None:
        console.write('已取消选择，未启动下载服务。');return
    address=select_address(args.host)
    if address is None:
        console.write('已取消，未启动下载服务。');return
    handler=handler_for(path,progress=True)
    with IPAServer((address,args.port),handler) as server:
        console.write('请让手机和电脑连接同一个可信局域网。若出现防火墙提示，仅允许正在使用的专用网络。')
        console.write('编号保存在本电脑，重开工具继续沿用；不同 IPA 内容使用不同编号，旧的 /1 固定链接停用。')
        console.write('本服务只提供当前选定的 IPA；同一网络中知道地址的人也能下载，用完请退出。')
        worker=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.1},daemon=True);worker.start()
        show_selection(server)
        try:
            while wait_action()!='quit':
                if not server.pause_selection():
                    console.write('手机正在传输文件，请等待传输结束后再按回车换 App。');continue
                try:
                    selected=select_ipa()
                    if selected is None:console.write('已取消选择，继续提供原应用。')
                    else:server.finish_selection(handler_for(selected,progress=True))
                finally:server.finish_selection()
                show_selection(server)
        except KeyboardInterrupt:pass
        finally:server.shutdown();worker.join(timeout=3)
    console.write('已关闭 IPA 下载服务。')

if __name__=='__main__':
    try:main()
    except KeyboardInterrupt:console.write('已取消，未继续启动下载服务。');sys.exit(130)
    except (OSError,ValueError,zipfile.BadZipFile) as e:console.write('无法启动：'+str(e));sys.exit(1)

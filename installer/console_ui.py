"""Dependency-free terminal progress. Measured work uses bars; waiting uses activity."""
import atexit, ctypes, os, shutil, sys, threading, time, unicodedata

RESET='\033[0m'
def width(s):return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in s)
def clip(s,n):
    out=''
    for c in str(s).replace('\n',' '):
        if width(out+c)>n-1:return out+'…'
        out+=c
    return out
def duration(s):
    s=max(0,int(s));return f'{s//60:02d}:{s%60:02d}'
def amount(n,unit):
    if unit!='bytes':return str(int(n))
    return f'{n/1024**2:.1f} MiB' if n<1024**3 else f'{n/1024**3:.2f} GiB'

class Console:
    def __init__(self):
        self.stream=None;self.live=False;self.color=False;self.rows=0
        self.tasks=[];self.lock=threading.RLock();self.stop=threading.Event();self.worker=None;self.last_draw=0
    def enable(self):
        self.stream=sys.stdout;self.live=bool(self.stream.isatty());self.color=self.live
        if os.name=='nt' and self.live:
            try:
                k=ctypes.windll.kernel32;k.GetStdHandle.restype=ctypes.c_void_p
                handle=k.GetStdHandle(-11);mode=ctypes.c_ulong()
                if not k.GetConsoleMode(ctypes.c_void_p(handle),ctypes.byref(mode)) or not k.SetConsoleMode(ctypes.c_void_p(handle),mode.value|4):
                    self.live=False;self.color=False
            except (OSError,AttributeError):self.live=False;self.color=False
    def clear(self):
        if self.live and self.rows:
            for j in range(self.rows):
                self.stream.write('\r\033[2K')
                if j+1<self.rows:self.stream.write('\033[1A')
            self.rows=0
    def lines(self):
        now=time.monotonic();active=[t for t in self.tasks if now-t.started>=t.delay]
        if not active:return []
        primary=next((t for t in active if t.total is not None),active[-1])
        cols=max(20,shutil.get_terminal_size((88,24)).columns-2)
        if primary.total is not None:
            ratio=min(1,primary.done/primary.total) if primary.total else 1
            slots=max(8,min(36,cols-17));filled=int(ratio*slots)
            bar='█'*filled+'░'*(slots-filled)
            first=f'  [{bar}] {ratio*100:5.1f}%'
            elapsed=max(.001,now-primary.started);rate=primary.done/elapsed
            eta='--:--' if not rate else duration((primary.total-primary.done)/rate)
            stats=amount(primary.done,primary.unit)+' / '+amount(primary.total,primary.unit)
            if primary.unit=='bytes':stats+=f'  {rate/1024**2:.1f} MiB/s'
            stats+=f'  用时 {duration(elapsed)}  预计 {eta}'
        else:
            slots=max(8,min(36,cols-21));pos=int((now-primary.started)*7)%slots
            bar='░'*pos+'█'+'░'*(slots-pos-1)
            first=f'  [{bar}] 等待响应'
            stats=f'已等待 {duration(now-primary.started)} · 活动指示，不代表完成比例'
        detail=active[-1].label
        if active[-1] is primary:detail=primary.detail or primary.label
        return [clip('  '+primary.label,cols),clip(first,cols),clip('  '+stats,cols),clip('  当前：'+detail,cols)]
    def draw(self):
        if not self.live:return
        self.last_draw=time.monotonic()
        self.clear();lines=self.lines()
        if lines:
            self.stream.write('\033[?25l'+'\n'.join('\033[36m'+s+RESET for s in lines));self.rows=len(lines)
        else:self.stream.write('\033[?25h')
        self.stream.flush()
    def tick(self):
        while not self.stop.wait(.16):
            with self.lock:
                if self.live:
                    if self.tasks or self.rows:self.draw()
                else:
                    for t in self.tasks:
                        now=time.monotonic()
                        if now-t.last_plain>=5 and now-t.started>=t.delay:
                            self.write(t.label+' · 已用时 '+duration(now-t.started));t.last_plain=now
    def write(self,text='',color=None):
        with self.lock:
            self.clear();stream=self.stream or sys.stdout
            stream.write(((color or '') if self.color else '')+str(text)+(RESET if color and self.color else '')+'\n');stream.flush()
            if self.live:self.draw()
    def add(self,task):
        with self.lock:
            self.tasks.append(task)
            if self.worker is None:
                self.worker=threading.Thread(target=self.tick,daemon=True);self.worker.start()
            if self.live:self.draw()
    def remove(self,task,success):
        with self.lock:
            self.clear()
            if task in self.tasks:self.tasks.remove(task)
            visible=time.monotonic()-task.started>=task.delay
            if visible and (not self.tasks or not success):
                state='完成' if success else '未完成'
                # Never mark an incomplete measured task as complete.
                if success and task.total is not None and task.done<task.total:state='结束（部分完成）'
                self.write('  '+('✓' if success else '×')+' '+task.label+' · '+state+' · '+duration(time.monotonic()-task.started),'\033[32m' if success else '\033[31m')
            if self.live:self.draw()
    def close(self):
        self.stop.set()
        with self.lock:
            self.clear()
            if self.live:self.stream.write('\033[?25h'+RESET);self.stream.flush()

console=Console();atexit.register(console.close)

class Task:
    def __init__(self,label,total=None,unit='steps',delay=0):
        self.label=label;self.total=total;self.unit=unit;self.delay=delay
        self.done=0;self.detail='';self.started=time.monotonic();self.last_plain=self.started
    def __enter__(self):console.add(self);return self
    def update(self,done,detail=''):
        with console.lock:
            self.done=done;self.detail=detail
            if console.live and time.monotonic()-console.last_draw>=.08:console.draw()
    def __exit__(self,typ,value,tb):console.remove(self,typ is None)

def command_label(args):
    words=[str(x) for x in args];cmd=' '.join(words[1:]);label='正在与手机通信'
    if 'twrp format data' in cmd:label='正在格式化 Android 数据，请保持连接'
    elif 'twrp unmount' in cmd or 'umount ' in cmd:label='正在卸载 Android 数据分区'
    elif 'sha256sum' in cmd:label='正在核对设备数据 SHA256'
    elif 'dd if=' in cmd and ' of=' in cmd:label='正在写入设备并同步存储'
    elif 'dd if=' in cmd:label='正在读取设备数据／写后校验'
    elif 'push' in words:label='正在上传当前数据块至手机内存'
    elif 'boot' in words:label='正在上传并临时启动 TWRP'
    elif 'reboot' in words:label='正在请求手机切换启动状态'
    elif 'devices' in words:label='正在检测 USB 设备'
    return label

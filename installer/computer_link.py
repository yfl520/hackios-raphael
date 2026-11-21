"""Explicit continuous usbmux connection. Identity stays in RAM; no pair records are read or written."""
from pathlib import Path
import argparse,plistlib,socket,struct,subprocess,sys,time
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
from console_ui import console,Task
from computer_link_transport import NativeUSBLink,query

def receive_exact(connection,size):
    output=bytearray()
    while len(output)<size:
        chunk=connection.recv(size-len(output))
        if not chunk:raise EOFError('usbmux closed')
        output.extend(chunk)
    return bytes(output)

def device_list():
    request=plistlib.dumps({'MessageType':'ListDevices','ClientVersionString':'HackiOS R12',
        'ProgName':'HackiOSConnector','kLibUSBMuxVersion':3})
    with socket.create_connection(('127.0.0.1',27015),timeout=2) as connection:
        connection.settimeout(2)
        connection.sendall(struct.pack('<IIII',16+len(request),1,8,1)+request)
        length,version,message,tag=struct.unpack('<IIII',receive_exact(connection,16))
        if not (16<=length<=1024*1024 and version==1 and message==8 and tag==1):
            raise ValueError('usbmux header')
        reply=plistlib.loads(receive_exact(connection,length-16))
    devices=reply.get('DeviceList',[])
    if not isinstance(devices,list):raise ValueError('usbmux devices')
    return [d.get('Properties',{}) for d in devices if isinstance(d,dict)]

def matches(devices,identity):
    return any(p.get('ConnectionType')=='USB' and p.get('SerialNumber')==identity for p in devices)

def connect():
    console.write('请保持 iOS 已到桌面、解锁亮屏。Apple Mobile Device 服务与苹果 USB 驱动须已安装。')
    console.write('持续连接无 3 分钟上限；拔线或协议错误会退出。建立连接前最多等待 30 秒。')
    # Check the host service before changing any firmware descriptor.
    try:device_list()
    except (OSError,ValueError,EOFError,plistlib.InvalidFileException):
        raise RuntimeError('Apple Mobile Device 服务未响应。请先用爱思安装苹果驱动并启动服务，再重试。') from None
    link=NativeUSBLink()
    try:
        with Task('检查手机连接能力'):
            link.run('oem','link-native-capabilities')
            capability=link.work/'capability.bin';link.run('get_staged',capability)
            data=capability.read_bytes()
            if len(data)!=32 or struct.unpack('<4Q',data)!=(1,1,180000,30000):
                raise RuntimeError('当前加载器不支持本包的持续连接模式，请先更新本包并启动 iOS。')
        with Task('读取当前 iOS 的 USB 身份（不保存）'):
            link.open();response=query(link,{'Request':'GetValue','Key':'UniqueDeviceID'})
            identity=response.get('Value')
            if not isinstance(identity,str) or len(identity)!=40 or any(c not in '0123456789abcdefABCDEF' for c in identity):
                raise RuntimeError('未取得有效的 iOS USB 身份，未切换连接。')
            del response
            # close() also removes all temporary mailbox files.
            link.close()
        # No subprocess output, argument echo, or identifiers are printed.
        with Task('切换至持续爱思连接'):
            control=NativeUSBLink()
            try:control.run('oem','link-native-persistent',identity)
            finally:control.close()
        until=time.monotonic()+38
        with Task('等待 Windows 苹果 USB 设备枚举'):
            while time.monotonic()<until:
                try:
                    if matches(device_list(),identity):
                        console.write('✓ 本机已被 Apple Mobile Device 服务识别。请在爱思中查看连接状态。','\033[32m')
                        console.write('本脚本可关闭，连接不会因关闭脚本或达到 3 分钟而自动退出。')
                        console.write('首次配对请在手机点“信任”；已配对时通常不会再弹。')
                        console.write('拔线可结束本轮连接；重新插线后再运行本入口。断线恢复仍需真机验收。')
                        console.write('当前不支持爱思实时屏幕；部分设备信息不完整。')
                        return
                except (OSError,ValueError,EOFError,plistlib.InvalidFileException):pass
                time.sleep(.5)
        raise RuntimeError('未完成苹果 USB 枚举。未握手的请求会退回调试模式；请检查驱动后再试，不要重复刷机。')
    finally:
        try:link.close()
        except (RuntimeError,OSError,subprocess.TimeoutExpired):pass

def main(verify=True):
    console.enable()
    console.write('\n  H A C K i O S  /  连接爱思 · 持续模式','\033[1;36m')
    console.write('  酷安：你说喜欢是装的  ','\033[1;97;44m')
    if verify:
        from installer import verify_package
        verify_package()
    try:connect()
    except (OSError,ValueError,EOFError,RuntimeError,subprocess.TimeoutExpired):
        console.write('连接未完成。请确认 iOS 已启动，调试接口使用 WinUSB，苹果接口已安装 Apple 驱动。','\033[33m')
        console.write('若当前接口已是苹果设备，可直接查看爱思；若是 libusbK 设备，本包 fastboot 可能无法识别。')
        console.write('不自动改驱动、不自动重启。排查方法见使用说明。')
        raise

if __name__=='__main__':
    parser=argparse.ArgumentParser(description='手动开启持续爱思连接，不设 3 分钟运行上限。')
    parser.parse_args()
    try:main()
    except (Exception,KeyboardInterrupt) as error:
        # Do not print arbitrary tool errors: a protocol or subprocess may contain identifiers.
        if isinstance(error,RuntimeError) and str(error) in (
            'Apple Mobile Device 服务未响应。请先用爱思安装苹果驱动并启动服务，再重试。',
            '当前加载器不支持本包的持续连接模式，请先更新本包并启动 iOS。',
            '未取得有效的 iOS USB 身份，未切换连接。',
            '未完成苹果 USB 枚举。未握手的请求会退回调试模式；请检查驱动后再试，不要重复刷机。'):
            console.write(str(error))
        else:console.write('已停止连接（'+type(error).__name__+'）。未自动重启或刷写分区。')
        raise SystemExit(1)

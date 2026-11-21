"""Read this device's radio inputs and customize a clean, immutable release template.

Never writes modem/NV partitions. Invalid or missing inputs select the offline payload.
"""
from pathlib import Path
import hashlib,json,os,re,shutil,struct,zlib

SIZES={'modemst1':0x800000,'modemst2':0x800000,'fsg':0x800000,'fsc':0x40000}
HEADER=struct.Struct('<8sII5Q4I')

def need(value,message):
    if not value:raise RuntimeError(message)

def digest(data):return hashlib.sha256(data).hexdigest()

def sha(path):
    with Path(path).open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def validate_input(name,data):
    need(name in SIZES and len(data)==SIZES[name],name+' 长度不符合当前机型')
    # fsc is a cookie partition and can legitimately be blank on this model.
    if name!='fsc':
        need(data.count(b'\0')!=len(data) and data.count(b'\xff')!=len(data),name+' 是空白数据')
    return digest(data)

def candidates(phone,name):
    need(name in SIZES,'不支持的无线分区')
    nodes=['/dev/block/by-name/'+name,'/dev/block/bootdevice/by-name/'+name]
    # Kernel PARTNAME works even when recovery did not create the normal by-name aliases.
    command="for d in /sys/class/block/*; do [ -f \"$d/uevent\" ] || continue; "
    command+=f"if grep -qx 'PARTNAME={name}' \"$d/uevent\"; then basename \"$d\"; fi; done"
    try:
        for device in phone.shell(command,20).splitlines():
            if re.fullmatch(r'sd[a-z][0-9]+',device.strip()):nodes.append('/dev/block/'+device.strip())
    except RuntimeError:pass
    return list(dict.fromkeys(nodes))

def collect(phone,session,serial,notice=lambda text:None):
    """No prior session, serial, account, or network cache is used as a fallback."""
    session=Path(session);items={};failed=[]
    need(bool(serial) and re.fullmatch(r'[A-Za-z0-9_.:-]+',serial),'设备标识无效')
    for name,n in SIZES.items():
        errors=[];good=False
        for node in candidates(phone,name):
            try:
                need(phone.size(node)==n,name+' 分区长度异常')
                data=phone.read(node,0,n);h=validate_input(name,data)
                need(phone.region_sha(node,0,n)==h,name+' 手机与电脑校验不一致')
                path=session/(name+'.before.img');tmp=path.with_suffix('.tmp')
                tmp.write_bytes(data);need(sha(tmp)==h,name+' 本地保存校验失败');os.replace(tmp,path)
                items[name]={'file':path.name,'bytes':n,'sha256':h,'source':node};good=True;break
            except (RuntimeError,OSError,ValueError) as e:
                errors.append(type(e).__name__+': '+str(e)[-240:])
        if not good:
            failed.append(name);items[name]={'available':False,'errors':errors}
    ready=not failed
    if ready:notice('已提取并核对本机四项无线资源，未改写原始 NV 分区。')
    else:notice('无线资源提取不完整（'+'、'.join(failed)+'）；继续安装。Wi-Fi 与蜂窝可能不可用，蓝牙保留原配置。可修复连接后重新更新本包。')
    return {'version':1,'serial':serial,'ready':ready,'items':items,'failed':failed}

def header(data):
    need(len(data)>=4096,'启动资源截断')
    values=HEADER.unpack_from(data)
    magic,ver,hs,total,po,pl,ro,rl,pc,rc,hc,flags=values
    need((magic,ver,hs,flags)==(b'RAPHIOS1',2,4096,1),'不支持的启动资源版本')
    need(total==len(data) and po==4096 and pl>0 and ro%4096==0 and ro>=po+pl and ro+rl==total,'启动资源边界错误')
    check=bytearray(data[:4096]);struct.pack_into('<I',check,64,0)
    need(zlib.crc32(check)==hc,'启动资源头校验失败')
    need(zlib.crc32(data[po:po+pl])==pc and zlib.crc32(data[ro:ro+rl])==rc,'启动资源内容校验失败')
    return values

def geometry(layout,values):
    _,_,_,total,po,pl,ro,rl,*_=values
    need(layout.get('version')==1 and layout.get('bundle_bytes')==total and
         layout.get('payload_bytes')==pl and layout.get('resident_offset')==ro and
         layout.get('resident_bytes')==rl,'无线模板几何参数不匹配')
    need(set(layout['slots'])==set(SIZES),'无线槽位缺失')
    spans=[]
    for name,n in SIZES.items():
        slot=layout['slots'][name];need(slot['bytes']==n,'无线槽位长度错误')
        at=0
        for s in slot['extents']:
            start,count=s['offset'],s['bytes']
            need(s['logical']==at and start>=0 and start%4096==0 and count>0 and count%4096==0 and start+count<=rl,'无线槽位越界或不连续')
            at+=count;spans.append((start,start+count))
        need(at==n,'无线槽位覆盖不完整')
    spans.sort();need(all(a[1]<=b[0] for a,b in zip(spans,spans[1:])),'无线槽位重叠')

def prepare(package,session,record,serial,notice=lambda text:None):
    """Template corruption is fatal; per-device NV failure degrades without blocking install."""
    package=Path(package);session=Path(session)
    layout=json.loads((package/'images/device-resource-map.json').read_text(encoding='utf-8'))
    template=package/'images/ios-boot-resources.bin'
    need(sha(template)==layout['template_sha256'],'启动模板损坏，请重新解压安装包')
    data=bytearray(template.read_bytes());values=header(data);geometry(layout,values)
    _,_,_,total,po,pl,ro,rl,*_=values
    # All shipping NV slots must be zero. Never silently reuse author or cached device data.
    for slot in layout['slots'].values():
        for span in slot['extents']:
            pos=ro+span['offset'];need(not any(data[pos:pos+span['bytes']]),'发行模板含非空身份数据，拒绝使用')
    inputs=record.get('wireless_inputs',{});ready=bool(inputs.get('ready'));reason=''
    if ready:
        try:
            need(record.get('serial')==serial==inputs.get('serial'),'本机无线记录属于其他设备')
            source={}
            need(set(inputs['items'])==set(SIZES),'本机无线记录缺失')
            for name,n in SIZES.items():
                row=inputs['items'][name];need(row.get('file')==name+'.before.img','本机无线文件名不正确')
                path=session/row['file'];need(path.resolve().is_relative_to(session.resolve()),'本机无线文件路径越界')
                raw=path.read_bytes();h=validate_input(name,raw)
                need(row.get('bytes')==n and row.get('sha256')==h,'本机无线文件校验失败：'+name);source[name]=raw
        except (RuntimeError,KeyError,TypeError,ValueError,OSError) as e:
            ready=False;reason=str(e)
    if ready:
        for name,raw in source.items():
            for span in layout['slots'][name]['extents']:
                pos=ro+span['offset'];at=span['logical'];count=span['bytes'];data[pos:pos+count]=raw[at:at+count]
    else:
        fallback=package/'images/ios-offline-payload.bin'
        need(fallback.stat().st_size==pl and sha(fallback)==layout['offline_payload_sha256'],'离线兜底启动资源损坏')
        data[po:po+pl]=fallback.read_bytes()
        notice('本轮使用离线兜底继续安装：Wi-Fi 与蜂窝可能不可用；不使用其他手机的 NV。')
    struct.pack_into('<II',data,56,zlib.crc32(data[po:po+pl]),zlib.crc32(data[ro:ro+rl]))
    struct.pack_into('<I',data,64,0);struct.pack_into('<I',data,64,zlib.crc32(data[:4096]));header(data)
    out=session/'device-boot-resources.bin';temp=out.with_suffix('.tmp');temp.write_bytes(data)
    expected=digest(data);need(sha(temp)==expected,'本机启动资源保存校验失败');os.replace(temp,out)
    record['device_resources']={'version':1,'serial':serial,'mode':'wireless' if ready else 'offline',
        'bytes':total,'sha256':expected,'file':out.name,'reason':reason,
        'template_sha256':layout['template_sha256']}
    return out,expected

"""Portable GPT/sparse validation. Pure host code, no device commands."""
import contextlib,hashlib,importlib.util,io,struct,uuid,zlib
BS=4096
DARWIN_BLOCKS=5243387
DATA_FIRST=2042368
SYSTEM_BYTES=2780090368
RESOURCE_OFFSET=0xc0000000
RESOURCE_BYTES=157548544
APFS=uuid.UUID('7c3457ef-0000-11aa-aa11-00306543ecac')
MISC=uuid.UUID('82acc91f-357c-4a68-9c8f-689e1b1a23a1')

def need(ok,message):
    if not ok:raise RuntimeError(message)

def digest(data):return hashlib.sha256(data).hexdigest()

def file_sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

GPT_WINDOW_MAX=16*1024**2

def gpt_header(b,my,alternate):
    need(len(b)==BS and b[:8]==b'EFI PART','GPT 签名不正确')
    rev,n,crc=struct.unpack_from('<III',b,8)
    need(rev==0x10000 and 92<=n<=BS,'GPT 版本/头长度不支持')
    check=bytearray(b[:n]);struct.pack_into('<I',check,16,0)
    need(zlib.crc32(check)==crc,'GPT 头 CRC 错误')
    here,other,first,last=struct.unpack_from('<QQQQ',b,24)
    table,count,entry,tablecrc=struct.unpack_from('<QIII',b,72)
    need((here,other)==(my,alternate),'GPT 主副位置不一致')
    blocks=max(my,alternate)+1
    need(3<=first<=last<blocks-1 and entry==128 and 0<count<=128,'GPT 表布局不支持')
    table_blocks=(count*entry+BS-1)//BS
    if my==1:
        need(2<=table and table+table_blocks<=first,'GPT 主表越过保留区域')
    else:
        need(last<table and table+table_blocks<=my,'GPT 副表越过保留区域')
    return dict(my=here,alternate=other,first_usable=first,last_usable=last,entries_lba=table,
                count=count,entry_size=entry,entries_crc=tablecrc,guid=b[56:72])

def gpt_window_sizes(primary,backup,size):
    need(size%BS==0 and size>=32*1024**3,'不支持的磁盘容量/扇区大小')
    blocks=size//BS
    ph=gpt_header(primary,1,blocks-1);bh=gpt_header(backup,blocks-1,1)
    need(all(ph[k]==bh[k] for k in ('first_usable','last_usable','count','entry_size','guid')),'GPT 主副元数据不一致')
    head=max(8*BS,(ph['entries_lba']+(ph['count']*128+BS-1)//BS)*BS)
    tail=max(8*BS,(blocks-bh['entries_lba'])*BS)
    need(head<=GPT_WINDOW_MAX and tail<=GPT_WINDOW_MAX,'GPT 表距离磁盘边界超过支持的 16 MiB；已停止，未修改布局')
    return head,tail

def parse_gpt(head,tail,size):
    need(len(head)%BS==len(tail)%BS==0 and min(len(head),len(tail))>=8*BS,'GPT 备份长度不正确')
    hs,ts=gpt_window_sizes(head[BS:2*BS],tail[-BS:],size)
    need(hs==len(head) and ts==len(tail),'GPT 备份范围与表头不一致')
    blocks=size//BS;tail_base=blocks-len(tail)//BS
    ph=gpt_header(head[BS:2*BS],1,blocks-1);bh=gpt_header(tail[-BS:],blocks-1,1)
    need(all(ph[k]==bh[k] for k in ('first_usable','last_usable','count','entry_size','guid')),'GPT 主副元数据不一致')
    n=ph['count']*ph['entry_size'];at=ph['entries_lba']*BS;bt=(bh['entries_lba']-tail_base)*BS
    need(2*BS<=at and at+n<=len(head) and 0<=bt and bt+n<=len(tail)-BS,'GPT 表不在已备份范围')
    need(ph['first_usable']>=ph['entries_lba']+(n+BS-1)//BS and ph['last_usable']<bh['entries_lba'],'GPT 可用范围覆盖分区表')
    raw=head[at:at+n]
    need(raw==tail[bt:bt+n] and zlib.crc32(raw)==ph['entries_crc']==bh['entries_crc'],'GPT 表内容/CRC 不一致')
    parts=[];names=set();guids=set()
    for i in range(ph['count']):
        e=raw[i*128:(i+1)*128];typ=uuid.UUID(bytes_le=e[:16])
        if not typ.int:continue
        guid=uuid.UUID(bytes_le=e[16:32]);first,last=struct.unpack_from('<QQ',e,32)
        name=e[56:128].decode('utf-16le').split('\0',1)[0]
        need(name and name not in names and guid.int and guid not in guids,'重复分区名称/GUID')
        need(ph['first_usable']<=first<=last<=ph['last_usable'],'分区越过 GPT 边界')
        names.add(name);guids.add(guid)
        parts.append(dict(index=i+1,name=name,first=first,last=last,blocks=last-first+1,type=str(typ),guid=str(guid)))
    ordered=sorted(parts,key=lambda p:p['first'])
    need(all(a['last']<b['first'] for a,b in zip(ordered,ordered[1:])),'分区重叠')
    result=dict(head=head,tail=tail,size=size,blocks=blocks,ph=ph,bh=bh,parts=parts,table=raw)
    return result

def part(g,name):
    hits=[p for p in g['parts'] if p['name']==name]
    need(len(hits)==1,'找不到唯一的分区：'+name)
    return hits[0]

def multiboot_indicators(g):
    """Positive GPT indicators only; EFI alone is not proof of Windows."""
    types={
        'ebd0a0a2-b9e5-4433-87c0-68b6b72699c7':'Microsoft 基本数据',
        'e3c9e316-0b5c-4db8-817d-f92df00215ae':'Microsoft 保留分区',
        'de94bba4-06d1-4d40-a16a-bfd50179d6ac':'Windows 恢复分区',
        'c12a7328-f81f-11d2-ba4b-00a0c93ec93b':'EFI 系统分区 / 第三方引导'}
    names={'windows','win','win10','win11','winarm','winre','esp','uefi','msr','woa'}
    result=[]
    for p in g['parts']:
        reason=types.get(p['type'].lower())
        if p['name'].lower() in names:reason=reason or 'Windows / UEFI 分区名称'
        if reason:result.append(dict(name=p['name'],type=p['type'],reason=reason))
    return result

def standard_layout_only(g):
    hits=multiboot_indicators(g)
    need(not hits,'检测到 Windows / EFI / 第三方多系统布局：'+
         '、'.join(p['name']+'（'+p['reason']+'）' for p in hits)+
         '。当前安装器尚不支持保留此布局，已停止，不会自动删除 Windows 或改写分区；请保留本轮分区记录。')

def compatible(g,require_darwin=True):
    standard_layout_only(g)
    m=part(g,'misc');r=part(g,'recovery');u=part(g,'userdata')
    need((m['first'],m['last'],m['type'])==(1024,2047,str(MISC)),'misc 布局与已验证引导器不匹配')
    need(r['blocks']*BS==64*1024**2,'recovery 必须是 64 MiB')
    if require_darwin:
        d=part(g,'darwin')
        need(d['type']==str(APFS) and 5242875<=d['blocks']<=(1024**4)//BS,'darwin 大小/类型与当前镜像不匹配')
        need(d['first']%512==0 and d['first']==u['last']+1 and d['last']<=g['ph']['last_usable'],'darwin 边界不匹配')
    return g

def capacity_limits(g):
    compatible(g,False)
    u=part(g,'userdata')
    minimum_android_end=((u['first']+8*1024**3//BS+511)//512)*512
    maximum=(u['last']+1-minimum_android_end)//(1024**3//BS)
    return 20,min(1024,maximum)

def validate_capacity(g,gib):
    lo,hi=capacity_limits(g)
    need(type(gib) is int and lo<=gib<=hi,f'iOS 容量请输入 {lo}～{hi} 的整数，单位 GiB')
    return gib

def make_plan(g,directory,tool,gib):
    validate_capacity(g,gib)
    compatible(g,False)
    need(not any(p['name']=='darwin' for p in g['parts']),'已存在 darwin，不能再次分区')
    directory.mkdir(parents=True,exist_ok=True)
    (directory/'gpt-head.bin').write_bytes(g['head']);(directory/'gpt-tail.bin').write_bytes(g['tail'])
    spec=importlib.util.spec_from_file_location('upstream_carve',tool)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    before_argv=module.sys.argv
    capture=io.StringIO()
    try:
        module.sys.argv=[str(tool),'--wipe',str(directory),str(gib)]
        with contextlib.redirect_stdout(capture):module.main()
    except SystemExit as e:raise RuntimeError('上游分区生成器拒绝当前布局：'+str(e)) from e
    finally:module.sys.argv=before_argv
    (directory/'upstream-plan.txt').write_text(capture.getvalue(),encoding='utf-8')
    new=parse_gpt((directory/'gpt-head.new').read_bytes(),(directory/'gpt-tail.new').read_bytes(),g['size'])
    compatible(new)
    u=part(g,'userdata');v=part(new,'userdata');d=part(new,'darwin')
    need(d['last']==u['last'] and d['first']>=u['first'],'iOS 分区越过原 userdata 范围')
    need(v['first']==u['first'] and v['blocks']*BS>=8*1024**3,'分区后 Android 空间不足 8 GiB')
    modified=[i for i in range(g['ph']['count']) if g['table'][i*128:(i+1)*128]!=new['table'][i*128:(i+1)*128]]
    need(modified==sorted([u['index']-1,d['index']-1]),'分区方案修改了无关条目')
    a=g['table'][(u['index']-1)*128:u['index']*128];b=new['table'][(u['index']-1)*128:u['index']*128]
    need(a[:40]==b[:40] and a[48:]==b[48:],'分区方案修改了 userdata 结束位置以外的内容')
    need(g['head'][:BS]==new['head'][:BS],'分区方案修改了 MBR')
    return new

def gpt_writes(g):
    """Complete one redundant copy at a time, table before header; leave MBR alone."""
    n=(g['ph']['count']*128+BS-1)//BS
    for key,blob,base in [('ph',g['head'],0),('bh',g['tail'],g['blocks']-len(g['tail'])//BS)]:
        table=g[key]['entries_lba'];start=(table-base)*BS
        yield table*BS,blob[start:start+n*BS]
        h=g[key]['my'];start=(h-base)*BS
        yield h*BS,blob[start:start+BS]

def sparse_chunks(path):
    """Yield initialized RAW/FILL extents only; DONT_CARE never means zero/write."""
    with path.open('rb') as f:
        head=f.read(28);need(len(head)==28,'稀疏镜像头不完整')
        magic,major,minor,fh,ch,bs,blocks,chunks,crc=struct.unpack('<IHHHHIIII',head)
        need(magic==0xed26ff3a and (major,minor)==(1,0) and fh>=28 and ch>=12 and bs==BS,'不支持的稀疏镜像格式')
        need(f.read(fh-28)==bytes(fh-28),'不支持的稀疏镜像扩展')
        position=0
        for _ in range(chunks):
            raw=f.read(ch);need(len(raw)==ch,'稀疏块头不完整')
            typ,res,n,total=struct.unpack_from('<HHII',raw)
            need(res==0 and total>=ch and position+n<=blocks,'稀疏块边界错误')
            length=n*BS;payload=total-ch
            if typ==0xcac1:
                need(payload==length,'RAW 块大小错误')
                left=length;at=position*BS
                while left:
                    b=f.read(min(left,32*1024**2));need(b,'RAW 块截断')
                    yield at,b;at+=len(b);left-=len(b)
            elif typ==0xcac2:
                need(payload==4,'FILL 块大小错误');word=f.read(4);need(len(word)==4,'FILL 块截断')
                left=length;at=position*BS
                while left:
                    nbytes=min(left,32*1024**2);yield at,word*(nbytes//4);at+=nbytes;left-=nbytes
            elif typ==0xcac3:need(payload==0,'DONT_CARE 不应包含数据')
            elif typ==0xcac4:
                need(n==0 and payload==4 and len(f.read(4))==4,'CRC 块错误')
            else:raise RuntimeError('未知稀疏块类型')
            position+=n
        need(position==blocks and not f.read(1),'稀疏镜像长度/尾部错误')

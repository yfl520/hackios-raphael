"""Verify source build products; never claim device acceptance."""
from pathlib import Path
import argparse,hashlib,json,plistlib,struct
ap=argparse.ArgumentParser();ap.add_argument('--complete',action='store_true');args=ap.parse_args()
root=Path(__file__).resolve().parents[1]
folder=root/'build/artifacts'
expected={'ath10k.o','kernel.macho','libOSMesa.8.dylib','loader.efi'}
expected.update('userland/'+x for x in ['BlueTool_raphael','HakiosAudio','HakiosAudio.Info.plist','hello','child','init','var_store','libhakios.dylib','libhakios.sb.dylib','libhakios.tweaks.dylib','libhakios.app.dylib','libhakios.media.dylib','libhakios.store.dylib','libhakios.fairplay.dylib','libhakios.gl.dylib'])
records=[];seen=set()
for p in sorted(folder.rglob('*')):
    if not p.is_file():continue
    rel=p.relative_to(folder).as_posix();seen.add(rel);data=p.read_bytes()
    assert data,(rel,'empty')
    if p.suffix=='.plist':
        assert isinstance(plistlib.loads(data),dict);kind='property list'
    elif data[:4]==b'\xcf\xfa\xed\xfe':
        assert len(data)>=32,(rel,'truncated Mach-O')
        _,cpu,subtype,filetype=struct.unpack_from('<IIII',data)
        assert cpu==0x100000c,(rel,'not ARM64')
        assert filetype in {1,2,6,8},(rel,'unexpected Mach-O type')
        kind='arm64 Mach-O'
    elif data[:2]==b'MZ':
        off=struct.unpack_from('<I',data,0x3c)[0]
        assert data[off:off+4]==b'PE\0\0'
        assert struct.unpack_from('<H',data,off+4)[0]==0xaa64
        assert struct.unpack_from('<H',data,off+24)[0]==0x20b
        assert struct.unpack_from('<H',data,off+24+68)[0]==10
        kind='arm64 PE/UEFI'
    else:raise ValueError((rel,'unrecognized build product'))
    records.append(dict(path=p.relative_to(root).as_posix(),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),format=kind))
if args.complete:assert seen==expected,dict(missing=sorted(expected-seen),unexpected=sorted(seen-expected))
out=root/'build/reports/products.json';out.parent.mkdir(parents=True,exist_ok=True)
out.write_text(json.dumps(dict(products=records,complete=args.complete,hardware_tested=False),indent=2)+'\n')
print(f'Checked {len(records)} products; complete={args.complete}; compilation/format verification only.')

"""Retrieve and verify a pinned, source-only build dependency."""
from pathlib import Path
import argparse,hashlib,json,tarfile,tempfile,urllib.request,shutil
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('name');args=ap.parse_args()
deps=ROOT/'xnu-deps'
entry=json.loads((deps/'source-lock.json').read_text())[args.name]
dest=deps/args.name
marker=dest/'.source-pin.json'
if marker.exists():
    assert json.loads(marker.read_text())==entry,'Dependency source pin changed; use a fresh build directory'
    print('Pinned dependency already present:',args.name)
    raise SystemExit(0)
if dest.exists():raise RuntimeError(f'Refusing to replace existing source directory: {dest}')
cache=deps/'downloads';cache.mkdir(exist_ok=True)
archive=cache/entry['filename']
if not archive.exists():
    req=urllib.request.Request(entry['url'],headers={'User-Agent':'RaphaelSourceBuild/1.0'})
    temp=archive.with_suffix(archive.suffix+'.partial')
    with urllib.request.urlopen(req,timeout=90) as r,temp.open('wb') as f:shutil.copyfileobj(r,f)
    assert hashlib.sha256(temp.read_bytes()).hexdigest()==entry['sha256'],'Source archive hash mismatch'
    temp.replace(archive)
assert hashlib.sha256(archive.read_bytes()).hexdigest()==entry['sha256'],'Cached source archive hash mismatch'
with tempfile.TemporaryDirectory(prefix='source-unpack-',dir=deps) as temporary:
    temporary=Path(temporary)
    with tarfile.open(archive,'r:gz') as tf:tf.extractall(temporary,filter='data')
    children=list(temporary.iterdir());assert len(children)==1 and children[0].is_dir()
    children[0].rename(dest)
marker.write_text(json.dumps(entry,indent=2)+'\n')
print('Verified source dependency:',args.name,entry['commit'])

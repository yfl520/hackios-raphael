"""Audit the public tree without emitting matched private values."""
from pathlib import Path
import re,hashlib,json,os
ROOT=Path(__file__).resolve().parents[1]
excluded_dirs={'.git','__pycache__','phone-logs','private-inputs','local-storage.h','BUILD','obj','cache','venv'}
excluded_ext={'.log','.apfs','.dmg','.img','.efi','.o','.obj','.a','.dylib','.so','.bin','.ipsw','.ipa','.pem','.key','.pyc'}
# setup.sh builds tcsh before this source link can resolve.
generated_links={'xnu-deps/host/bin/csh':'tcsh'}
patterns={
 'api_key':re.compile(rb'sk-[A-Za-z0-9]{20,}'),
 'private_key':re.compile(rb'-----BEGIN (?:OPENSSH |RSA |EC |ENCRYPTED )?PRIVATE KEY-----'),
 'github_token':re.compile(rb'(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})'),
 'developer_path':re.compile(rb'/home/[a-z_][\w-]*/(?:hakios|raphael|bluetooth)|[A-Za-z]:[\\/](?:Users[\\/][^\\/\s]+|Gpt)'),
}
marker_path=os.environ.get('PRIVATE_MARKERS_FILE')
if marker_path:
    for i,value in enumerate(json.loads(Path(marker_path).read_text())):
        patterns[f'private_marker_{i}']=re.compile(re.escape(value.encode()),re.I)
findings=[];files=[]
def source_paths():
    for current, directories, names in os.walk(ROOT, followlinks=False):
        current=Path(current)
        for name in list(directories):
            p=current/name
            if name=='.git' or (current==ROOT and name=='build'):
                directories.remove(name)
                if name=='.git' and current!=ROOT:
                    findings.append(dict(path=p.relative_to(ROOT).as_posix(),kind='nested_git'))
            elif p.is_symlink():
                directories.remove(name)
                yield p
        for name in names:
            if current==ROOT and name=='.git':continue
            yield current/name

for p in sorted(source_paths()):
    rel=p.relative_to(ROOT)
    if rel.parts[0] == 'build':continue
    if p.is_symlink():
        # A source-local symbolic link is expected; no external target is permitted.
        if not p.resolve().is_relative_to(ROOT.resolve()):findings.append(dict(path=rel.as_posix(),kind='external_link'))
        elif not p.exists() and generated_links.get(rel.as_posix())!=os.readlink(p):
            findings.append(dict(path=rel.as_posix(),kind='broken_link'))
        continue
    if not p.is_file():continue
    if any(n in excluded_dirs for n in rel.parts) or p.suffix.lower() in excluded_ext:
        findings.append(dict(path=rel.as_posix(),kind='excluded_file_type'))
    data=p.read_bytes()
    if b'\0' in data:findings.append(dict(path=rel.as_posix(),kind='binary_file'))
    for label,pattern in patterns.items():
        if pattern.search(data):findings.append(dict(path=rel.as_posix(),kind=label))
    if rel.parts[0] != 'release-metadata':
        files.append((rel.as_posix(),hashlib.sha256(data).hexdigest(),len(data)))
meta=ROOT/'build/reports/source-audit';meta.mkdir(parents=True,exist_ok=True)
(meta/'privacy-audit.json').write_text(json.dumps(dict(passed=not findings,files_scanned=len(files),findings=findings,scope='File types, private-key/API patterns, known developer path and device identifier; no matched values emitted'),indent=2)+'\n')
(meta/'SHA256SUMS').write_text(''.join(f'{sha}  {path}\n' for path,sha,_ in files))
print(json.dumps(dict(passed=not findings,files=len(files),source_bytes=sum(n for _,_,n in files),findings=findings),indent=2))
raise SystemExit(bool(findings))

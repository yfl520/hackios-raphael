"""Reproduce the pinned public runtime and verify every adapted source file.

This prepares source only. It does not access a phone or assemble an image.
"""
from pathlib import Path
import argparse, hashlib, json, shutil, subprocess, tarfile, tempfile, urllib.request

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, help='Use an already downloaded pinned archive')
    parser.add_argument('--output', type=Path, default=HERE/'translator')
    args = parser.parse_args()
    lock = json.loads((HERE/'metal2vulkan-lock.json').read_text())
    target = args.output.resolve()
    if target.exists():
        parser.error('Output already exists; choose a new directory')
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='raphael-runtime-', dir=target.parent) as temp:
        temp = Path(temp)
        archive = args.archive
        if archive is None:
            archive = temp/'upstream.tar.gz'
            with urllib.request.urlopen(lock['archive_url'], timeout=60) as source, archive.open('wb') as dest:
                shutil.copyfileobj(source, dest)
        with archive.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != lock['archive_sha256']:
            raise ValueError('Upstream archive SHA-256 mismatch')
        source_dir = temp/'source'
        source_dir.mkdir()
        with tarfile.open(archive) as tar:
            tar.extractall(source_dir, filter='data')
        roots = list(source_dir.iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise ValueError('Unexpected source archive layout')
        runtime = roots[0]
        patch = HERE/'metal2vulkan-ios16.patch'
        subprocess.run(['patch', '-p1', '--batch', '--forward', '-i', str(patch)], cwd=runtime, check=True)
        for row in lock['source_changes']:
            path = runtime/row['path']
            if hashlib.sha256(path.read_bytes()).hexdigest() != row['after']:
                raise ValueError('Adapted source hash mismatch: '+row['path'])
        shutil.move(str(runtime), target)
    print(f'Prepared runtime {lock["commit"]}; verified {len(lock["source_changes"])} adapted files')

if __name__ == '__main__':
    main()

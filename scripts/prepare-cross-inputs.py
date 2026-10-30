"""Prepare pinned cctools and SDK in a new user-selected directory."""
from pathlib import Path
import argparse,hashlib,json,tarfile,urllib.request,shutil,subprocess
ROOT=Path(__file__).resolve().parents[1]
ap=argparse.ArgumentParser();ap.add_argument('destination',type=Path);args=ap.parse_args()
dest=args.destination.resolve()
if dest.exists():raise RuntimeError('Use a new directory; existing toolchains are never overwritten')
dest.mkdir(parents=True)
lock=json.loads((ROOT/'xnu-deps/cross-inputs.json').read_text())
archive=dest/'iOSToolchain-x86_64.tar.xz'
with urllib.request.urlopen(lock['toolchain']['url'],timeout=120) as r,archive.open('wb') as f:shutil.copyfileobj(r,f)
with archive.open('rb') as f:actual=hashlib.file_digest(f,'sha256').hexdigest()
assert actual==lock['toolchain']['sha256'],'Toolchain archive hash mismatch'
(dest/'toolchain').mkdir()
with tarfile.open(archive,'r:xz') as tf:tf.extractall(dest/'toolchain',filter='data')
assert (dest/'toolchain/linux/iphone/bin/ld').is_file()
repo=dest/'sdks-git';repo.mkdir()
def git(*args):subprocess.run(['git','-C',str(repo),*args],check=True)
git('init','-q');git('remote','add','origin',lock['ios_sdk']['url'])
git('config','core.sparseCheckout','true')
(repo/'.git/info/sparse-checkout').write_text('/iPhoneOS16.5.sdk/\n')
git('fetch','--depth','1','--filter=blob:none','origin',lock['ios_sdk']['commit'])
git('checkout','--detach','FETCH_HEAD')
assert subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()==lock['ios_sdk']['commit']
(dest/'sdks').mkdir()
(dest/'sdks/iPhoneOS16.5.sdk').symlink_to('../sdks-git/iPhoneOS16.5.sdk')
assert (dest/'sdks/iPhoneOS16.5.sdk/SDKSettings.plist').is_file()
print('Pinned cross inputs prepared. Set THEOS to the destination shown:',dest)

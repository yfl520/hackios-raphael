"""Run the public checker against synthetic product directories, without SDKs."""
from pathlib import Path
import json,plistlib,shutil,struct,subprocess,sys,tempfile,unittest

CHECKER=Path(__file__).resolve().parents[1]/'scripts/verify-products.py'

def macho(cpu=0x100000c,filetype=2):
    return struct.pack('<8I',0xfeedfacf,cpu,0,filetype,0,0,0,0)

class ProductChecks(unittest.TestCase):
    def setUp(self):
        temp=tempfile.TemporaryDirectory();self.addCleanup(temp.cleanup)
        self.root=Path(temp.name);self.folder=self.root/'build/artifacts'
        (self.root/'scripts').mkdir();shutil.copyfile(CHECKER,self.root/'scripts/verify-products.py')
        self.put('ath10k.o',macho(filetype=1));self.put('kernel.macho',macho())
        self.put('libOSMesa.8.dylib',macho(filetype=6))
        pe=bytearray(256);pe[:2]=b'MZ';struct.pack_into('<I',pe,0x3c,64)
        pe[64:68]=b'PE\0\0';struct.pack_into('<H',pe,68,0xaa64)
        struct.pack_into('<H',pe,88,0x20b);struct.pack_into('<H',pe,156,10)
        self.put('loader.efi',pe)
        for name in ('BlueTool_raphael','HakiosAudio','hello','child','init','var_store',
                     'libhakios.dylib','libhakios.sb.dylib','libhakios.tweaks.dylib',
                     'libhakios.app.dylib','libhakios.media.dylib','libhakios.store.dylib',
                     'libhakios.fairplay.dylib','libhakios.gl.dylib'):
            self.put('userland/'+name,macho(filetype=6 if name.endswith('.dylib') else 2))
        self.put('userland/HakiosAudio.Info.plist',plistlib.dumps({'CFBundleIdentifier':'synthetic.audio'}))

    def put(self,name,data):
        path=self.folder/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data)

    def run_checker(self):
        return subprocess.run([sys.executable,'-B',str(self.root/'scripts/verify-products.py'),'--complete'],capture_output=True,text=True)

    def test_core_build_does_not_require_optional_components(self):
        result=self.run_checker();self.assertEqual(result.returncode,0,result.stderr)
        report=json.loads((self.root/'build/reports/products.json').read_text())
        self.assertEqual(len(report['products']),19);self.assertFalse(report['hardware_tested'])

    def test_optional_components_are_validated_and_reported(self):
        self.put('userland/libhakios.cellular-data.dylib',macho(filetype=6))
        self.put('persistence/init.persistence',macho())
        result=self.run_checker();self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(len(json.loads((self.root/'build/reports/products.json').read_text())['products']),21)

    def test_optional_components_do_not_hide_missing_core_product(self):
        self.put('persistence/init.persistence',macho());(self.folder/'kernel.macho').unlink()
        self.assertNotEqual(self.run_checker().returncode,0)

    def test_unrecognized_extra_arm64_product_is_rejected(self):
        self.put('userland/unexpected.dylib',macho(filetype=6))
        self.assertNotEqual(self.run_checker().returncode,0)

    def test_optional_product_with_wrong_architecture_is_rejected(self):
        self.put('userland/libhakios.cellular-data.dylib',macho(cpu=0x1000007,filetype=6))
        self.assertNotEqual(self.run_checker().returncode,0)

    def test_optional_product_with_invalid_format_is_rejected(self):
        self.put('persistence/init.persistence',b'synthetic invalid product')
        self.assertNotEqual(self.run_checker().returncode,0)

if __name__=='__main__':unittest.main()

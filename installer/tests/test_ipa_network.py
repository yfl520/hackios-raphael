from pathlib import Path
from unittest.mock import patch
import contextlib,io,os,re,socket,subprocess,sys,tempfile,time,unittest,urllib.request,zipfile
W=Path(__file__).resolve().parent;sys.path.insert(0,str(W.parent))
import ipa_url as tool

def adapter(address,name='以太网',index=9,physical=True,up=True,gateway=True):
    return dict(address=address,name=name,index=index,physical=physical,up=up,gateway=gateway,prefix=24)

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=W);self.addCleanup(self.tmp.cleanup)
        self.index=Path(self.tmp.name)/'HackiOS'/'ipa-links.json'
        self.index_patch=patch.object(tool,'default_index_path',return_value=self.index)
        self.index_patch.start();self.addCleanup(self.index_patch.stop)

    def test_filters_observed_vmware_vpn_disconnected_and_duplicate_addresses(self):
        physical=adapter('192.168.5.12')
        rows=[adapter('192.168.233.1','VMnet1',16,False),adapter('192.168.70.1','VMnet8',4,False),
              adapter('172.18.0.1','singbox_tun',18,False),adapter('169.254.214.153','WLAN',6,True,False),
              physical,physical.copy(),adapter('127.0.0.1'),adapter('224.0.0.1'),adapter('0.0.0.0')]
        self.assertEqual(tool.lan_candidates(rows),[physical])

    def test_single_physical_is_chosen_without_input_even_when_vpn_has_gateway(self):
        rows=[adapter('172.18.0.1','VPN',1,False),adapter('192.168.5.12')]
        with patch.object(tool,'network_interfaces',return_value=rows),patch('builtins.input') as prompt,\
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.select_address(),'192.168.5.12');prompt.assert_not_called()

    def test_multiple_physical_adapters_allow_selecting_wifi_and_prioritize_gateway(self):
        rows=[adapter('10.1.0.2','直连',2,gateway=False),adapter('192.168.5.12','以太网',9),
              adapter('192.168.6.2','Wi-Fi',10)]
        with patch.object(tool,'network_interfaces',return_value=rows),patch('builtins.input',return_value='2'),\
             contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.select_address(),'192.168.6.2')

    def test_manual_can_choose_assigned_shared_network_but_rejects_phone_or_gateway(self):
        rows=[adapter('192.168.137.1','共享热点',4,False)]
        with patch.object(tool,'network_interfaces',return_value=rows),\
             patch('builtins.input',side_effect=['192.168.137.2','192.168.137.1']),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(tool.select_address(),'192.168.137.1')
        with patch.object(tool,'network_interfaces',return_value=rows):
            with self.assertRaises(ValueError):tool.select_address('192.168.137.2')

    def test_query_failure_allows_manual_address_and_cancel(self):
        for failure in (OSError('synthetic'),subprocess.TimeoutExpired('powershell',20)):
            with self.subTest(failure=type(failure).__name__),\
                 patch.object(tool,'network_interfaces',side_effect=failure),\
                 patch('builtins.input',side_effect=['169.254.1.2','192.168.5.12']),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(tool.select_address(),'192.168.5.12')
        with patch.object(tool,'network_interfaces',return_value=[]),patch('builtins.input',return_value='Q'),\
             contextlib.redirect_stdout(io.StringIO()):self.assertIsNone(tool.select_address())

    @unittest.skipUnless(tool.os.name=='nt','Windows only')
    def test_actual_windows_query_returns_a_single_recommendation_on_this_host(self):
        rows=tool.network_interfaces()
        self.assertTrue(all({'address','name','up','physical','gateway','prefix','index'}<=set(r) for r in rows))
        candidates=tool.lan_candidates(rows)
        self.assertTrue(candidates)
        self.assertTrue(all(r['physical'] and r['up'] and tool.lan_ipv4(r['address']) for r in candidates))

    @unittest.skipUnless(tool.os.name=='nt','Windows only')
    def test_actual_cli_binds_selected_lan_and_prints_only_one_url(self):
        row=tool.lan_candidates(tool.network_interfaces())[0]
        with socket.socket() as probe:
            probe.bind((row['address'],0));port=probe.getsockname()[1]
        package=W.parent;ipa=Path(self.tmp.name)/'synthetic.ipa'
        with zipfile.ZipFile(ipa,'w') as archive:
            archive.writestr('Payload/Synthetic.app/Info.plist','<plist><dict/></plist>')
        route=tool.handler_for(ipa).download_path
        child=subprocess.Popen([sys.executable,'-u','-B','-X','utf8',str(package/'ipa_url.py'),str(ipa),
            '--host',row['address'],'--port',str(port)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),env=dict(os.environ,LOCALAPPDATA=self.tmp.name))
        opener=urllib.request.build_opener(urllib.request.ProxyHandler({}));ready=False
        try:
            end=time.monotonic()+10
            while time.monotonic()<end and child.poll() is None:
                try:
                    with opener.open(urllib.request.Request(f"http://{row['address']}:{port}{route}",method='HEAD'),timeout=.5) as response:
                        self.assertEqual(int(response.headers['Content-Length']),ipa.stat().st_size);ready=True;break
                except OSError:time.sleep(.1)
        finally:
            if child.poll() is None:child.terminate()
            output,error=child.communicate(timeout=5)
        self.assertTrue(ready,error.decode('utf-8','replace'))
        urls=re.findall(r'http://[^\s]+',output.decode('utf-8','replace'))
        self.assertEqual(urls,[f"http://{row['address']}:{port}{route}"])

if __name__=='__main__':unittest.main()

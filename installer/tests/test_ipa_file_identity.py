from pathlib import Path
from unittest.mock import patch
import concurrent.futures,contextlib,hashlib,io,json,os,subprocess,sys,tempfile,threading,unittest,urllib.error,urllib.request
W=Path(__file__).resolve().parent;sys.path.insert(0,str(W.parent))
import ipa_url as tool
from http.server import ThreadingHTTPServer

class Tests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=W);self.addCleanup(self.tmp.cleanup)
        self.index=Path(self.tmp.name)/'state'/'ipa-links.json'
        self.index_patch=patch.object(tool,'default_index_path',return_value=self.index)
        self.index_patch.start();self.addCleanup(self.index_patch.stop)

    def test_different_contents_use_different_routes_even_with_same_name_and_length(self):
        with tempfile.TemporaryDirectory(dir=W) as tmp:
            p=Path(tmp)/'应用.ipa';p.write_bytes(b'first-app')
            first=tool.handler_for(p);p.write_bytes(b'other-app');second=tool.handler_for(p)
            self.assertNotEqual(first.download_path,second.download_path)
            self.assertEqual(first.download_path,'/2');self.assertEqual(second.download_path,'/3')
            self.assertEqual(tool.handler_for(p).download_path,second.download_path)

    def test_another_process_and_another_filename_reuse_same_content_id(self):
        fingerprint=hashlib.sha256(b'one app').hexdigest()
        number=tool.file_number(fingerprint)
        env=dict(os.environ,LOCALAPPDATA=str(Path(self.tmp.name)/'user'))
        script='import sys;sys.path.insert(0,sys.argv[1]);import ipa_url;print(ipa_url.file_number(sys.argv[2],sys.argv[3]))'
        result=subprocess.run([sys.executable,'-B','-X','utf8','-c',script,str(W.parent),
                               fingerprint,str(self.index)],capture_output=True,text=True,env=env,timeout=5)
        self.assertEqual(result.returncode,0,result.stderr);self.assertEqual(int(result.stdout),number)
        a=Path(self.tmp.name)/'a.ipa';b=Path(self.tmp.name)/'不同文件名.ipa'
        a.write_bytes(b'one app');b.write_bytes(b'one app')
        self.assertEqual(tool.handler_for(a).download_path,tool.handler_for(b).download_path)

    def test_concurrent_processes_never_assign_one_number_to_different_apps(self):
        script='import sys;sys.path.insert(0,sys.argv[1]);import ipa_url;print(ipa_url.file_number(sys.argv[2],sys.argv[3]))'
        def launch(n):
            result=subprocess.run([sys.executable,'-B','-X','utf8','-c',script,str(W.parent),
                hashlib.sha256(str(n).encode()).hexdigest(),str(self.index)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr);return int(result.stdout)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:values=list(pool.map(launch,range(8)))
        self.assertEqual(sorted(values),list(range(2,10)))
        state=json.loads(self.index.read_text());self.assertEqual(state['next_id'],10)

    def test_corrupt_index_never_silently_restarts_from_old_ids(self):
        self.index.parent.mkdir();self.index.write_text('{broken')
        with self.assertRaisesRegex(ValueError,'不会重置编号'):
            tool.file_number(hashlib.sha256(b'one app').hexdigest())
        self.assertEqual(self.index.read_text(),'{broken')

    def test_current_route_serves_correct_bytes_old_route_returns_404_and_no_cache(self):
        with tempfile.TemporaryDirectory(dir=W) as tmp:
            first=Path(tmp)/'first.ipa';second=Path(tmp)/'second.ipa'
            first.write_bytes(b'old application');second.write_bytes(b'new application')
            old=tool.handler_for(first);new=tool.handler_for(second)
            with ThreadingHTTPServer(('127.0.0.1',0),new) as server:
                thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.05},daemon=True);thread.start()
                base='http://127.0.0.1:'+str(server.server_port)
                try:
                    with urllib.request.urlopen(base+new.download_path,timeout=3) as response:
                        self.assertEqual(response.read(),second.read_bytes())
                        self.assertIn('no-store',response.headers['Cache-Control'])
                        self.assertEqual(response.headers['X-Content-SHA256'],hashlib.sha256(second.read_bytes()).hexdigest())
                        self.assertIn(new.download_path[1:],response.headers['Content-Disposition'])
                    for route in (old.download_path,'/1','/1.ipa'):
                        with self.subTest(route=route),self.assertRaises(urllib.error.HTTPError) as error:
                            urllib.request.urlopen(base+route,timeout=3)
                        self.assertEqual(error.exception.code,404)
                    second.write_bytes(b'changed after selecting')
                    with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(base+new.download_path,timeout=3)
                    self.assertEqual(error.exception.code,409)
                finally:server.shutdown();thread.join(timeout=2)

    def test_switches_file_on_same_server_old_id_stops_serving_and_cancel_keeps_selection(self):
        first=Path(self.tmp.name)/'first.ipa';second=Path(self.tmp.name)/'second.ipa'
        first.write_bytes(b'first application');second.write_bytes(b'other application')
        old=tool.handler_for(first);new=tool.handler_for(second)
        with tool.IPAServer(('127.0.0.1',0),old) as server:
            thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.05},daemon=True);thread.start()
            base='http://127.0.0.1:'+str(server.server_port)
            try:
                with urllib.request.urlopen(base+old.download_path) as response:self.assertEqual(response.read(),first.read_bytes())
                self.assertTrue(server.pause_selection())
                with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(base+old.download_path)
                self.assertEqual(error.exception.code,503)
                server.finish_selection()  # Cancelling the picker resumes the original file.
                with urllib.request.urlopen(base+old.download_path) as response:self.assertEqual(response.read(),first.read_bytes())
                self.assertTrue(server.pause_selection());server.finish_selection(new)
                with self.assertRaises(urllib.error.HTTPError) as error:urllib.request.urlopen(base+old.download_path)
                self.assertEqual(error.exception.code,404)
                with urllib.request.urlopen(base+new.download_path) as response:self.assertEqual(response.read(),second.read_bytes())
                self.assertTrue(server.begin_transfer(new));self.assertFalse(server.pause_selection());server.end_transfer()
            finally:server.shutdown();thread.join(timeout=2)

    def test_progress_tracks_actual_hash_bytes_and_actual_range_bytes(self):
        path=Path(self.tmp.name)/'large.ipa';data=b'x'*(5*1024**2+19);path.write_bytes(data)
        tasks=[]
        class Meter:
            def __init__(self,label,total,unit):self.label=label;self.total=total;self.done=[];tasks.append(self)
            def __enter__(self):return self
            def __exit__(self,*args):pass
            def update(self,done,detail=''):self.done.append(done)
        with patch.object(tool,'Task',Meter),contextlib.redirect_stdout(io.StringIO()):
            handler=tool.handler_for(path,progress=True)
            with tool.IPAServer(('127.0.0.1',0),handler) as server:
                thread=threading.Thread(target=server.serve_forever,kwargs={'poll_interval':.05},daemon=True);thread.start()
                url='http://127.0.0.1:'+str(server.server_port)+handler.download_path
                try:
                    with urllib.request.urlopen(urllib.request.Request(url,headers={'Range':'bytes=17-100'})) as response:
                        self.assertEqual(response.read(),data[17:101])
                finally:server.shutdown();thread.join(timeout=2)
        self.assertEqual(len(tasks),2);self.assertEqual(tasks[0].total,len(data))
        self.assertEqual(tasks[0].done[-1],len(data));self.assertGreater(len(tasks[0].done),1)
        self.assertEqual(tasks[1].total,84);self.assertEqual(tasks[1].done,[84])

    def test_main_changes_files_and_cancels_in_one_window_without_gui(self):
        a=Path(self.tmp.name)/'first.ipa';b=Path(self.tmp.name)/'other.ipa';a.write_bytes(b'app1');b.write_bytes(b'app2')
        output=io.StringIO();actions=iter(['next','next','quit'])
        with patch.object(tool,'select_ipa',side_effect=[a,b,None]),patch.object(tool,'select_address',return_value='127.0.0.1'),\
             patch.object(tool,'wait_action',side_effect=lambda:next(actions)),\
             patch.object(sys,'argv',['ipa_url.py','--port','0']),contextlib.redirect_stdout(output):tool.main()
        self.assertIn('当前应用：#2 → first.ipa',output.getvalue())
        self.assertEqual(output.getvalue().count('当前应用：#3 → other.ipa'),2)
        self.assertIn('已取消选择，继续提供原应用',output.getvalue())
        self.assertIn('已关闭 IPA 下载服务',output.getvalue())

if __name__=='__main__':unittest.main()

"""Exercise the shipped adb.exe against an isolated local legacy ADB stub.

The OS assigns a private ephemeral port. Never connects to the real ADB server.
"""
from pathlib import Path
from unittest.mock import patch
import os,socket,struct,subprocess,tempfile,threading,unittest,sys

W=Path(__file__).resolve().parent;P=W.parent
sys.path.insert(0,str(P))
import installer as i
BB=Path(os.environ.get('HACKIOS_TEST_BUSYBOX','/nonexistent/busybox'))

@unittest.skipUnless(BB.is_file() and os.environ.get('HACKIOS_TEST_ADB'), 'Set HACKIOS_TEST_ADB and HACKIOS_TEST_BUSYBOX for legacy client tests')
class AdbClientTests(unittest.TestCase):
    def exchange(self,command,truncate=False):
        services=[];errors=[]
        with tempfile.TemporaryDirectory(dir=W) as temp, socket.socket() as server:
            server.bind(('127.0.0.1',0));server.listen();server.settimeout(10)
            port=server.getsockname()[1];done=threading.Event()
            def exact(conn,n):
                data=b''
                while len(data)<n:
                    part=conn.recv(n-len(data))
                    if not part:raise EOFError('ADB client closed request')
                    data+=part
                return data
            def serve():
                try:
                    while not done.is_set():
                        conn,_=server.accept()
                        with conn:
                            conn.settimeout(10)
                            while True:
                                service=exact(conn,int(exact(conn,4),16)).decode()
                                services.append(service)
                                if service=='host:version':conn.sendall(b'OKAY00040029');break
                                if service in ('host:host-features','host:features','host-serial:LEGACY:features'):
                                    conn.sendall(b'OKAY0000');break
                                if service=='host:transport:LEGACY':conn.sendall(b'OKAY');continue
                                if service=='host:tport:serial:LEGACY':conn.sendall(b'OKAY'+struct.pack('<Q',1));continue
                                if service.startswith('exec:'):
                                    conn.sendall(b'OKAY')
                                    result=subprocess.run([str(BB),'sh','-c',service[5:]],cwd=temp,
                                        env=dict(os.environ,TMPDIR=temp),capture_output=True,timeout=10)
                                    data=result.stdout+result.stderr
                                    conn.sendall(data[:-40] if truncate else data);done.set();break
                                raise AssertionError('Unexpected service: '+service)
                except BaseException as exc:errors.append(exc);done.set()
            thread=threading.Thread(target=serve,daemon=True);thread.start()
            phone=i.Phone();phone.adb=Path(os.environ['HACKIOS_TEST_ADB']);phone.serial='LEGACY';calls=[]
            def isolated_run(args,timeout,binary):
                args=list(map(str,args));self.assertEqual(args[args.index('-P')+1],'5037')
                args[args.index('-P')+1]=str(port);calls.append(args)
                result=subprocess.run(args,capture_output=True,timeout=15)
                self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
                return result.stdout
            try:
                with patch.object(i,'run',side_effect=isolated_run),patch.object(i,'record_command'):
                    output=phone.shell(command,binary=True)
            finally:
                done.set();thread.join(timeout=12)
            self.assertFalse(thread.is_alive());self.assertEqual(errors,[])
            self.assertEqual(len(calls),1)
            self.assertEqual(len([s for s in services if s.startswith('exec:')]),1)
            return output

    def test_actual_bundled_adb_preserves_legacy_binary_and_shell_quoting(self):
        output=self.exchange("printf '%s' 'quotes '\"'\"' and $literal `literal`'; printf '\\000\\001\\015\\012\\377'; printf 'warning' >&2")
        self.assertEqual(output,b"quotes ' and $literal `literal`\x00\x01\r\n\xff")

    def test_actual_bundled_adb_does_not_hide_remote_failure(self):
        with self.assertRaisesRegex(RuntimeError,'退出码 9'):
            self.exchange("printf partial; printf 'remote failure' >&2; exit 9")

    def test_actual_bundled_adb_zero_exit_with_truncated_reply_is_not_replayed(self):
        with self.assertRaisesRegex(RuntimeError,'完成标记缺失'):
            self.exchange("printf '3.7.0_12-EROFS Helium_Studio\\n'",truncate=True)

if __name__=='__main__':unittest.main()

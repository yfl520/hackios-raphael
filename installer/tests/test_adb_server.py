"""Cold local daemon test uses an ephemeral port and excludes all real USB phones."""
from pathlib import Path
from unittest.mock import patch
import contextlib,io,json,os,socket,subprocess,sys,unittest
W=Path(__file__).resolve().parent;P=W.parent;sys.path.insert(0,str(P))
import installer as i

class ServerTests(unittest.TestCase):
    def test_local_start_precedes_remote_style_enumeration_and_runs_once(self):
        p=i.Phone()
        with patch.object(i,'run',side_effect=['','List of devices attached\nMOCK\tdevice\n','List of devices attached\nMOCK\tdevice\n']) as run:
            self.assertEqual(p.adb_devices(),[['MOCK','device']]);p.adb_devices()
        calls=[call.args[0][1:] for call in run.call_args_list]
        self.assertEqual(calls,[['-L','tcp:5037','start-server'],['-H','127.0.0.1','-P','5037','devices'],['-H','127.0.0.1','-P','5037','devices']])
        self.assertTrue(p.adb_server_started)

    def test_start_failure_does_not_enumerate_or_claim_started(self):
        p=i.Phone()
        with patch.object(i,'run',side_effect=RuntimeError('port occupied')) as run:
            with self.assertRaisesRegex(RuntimeError,'port occupied'):p.adb_devices()
        self.assertEqual(run.call_count,1);self.assertFalse(p.adb_server_started)

    def test_verification_only_never_starts_adb_or_connects_to_phone(self):
        with patch.object(i.sys,'argv',['installer.py','--verify']),patch.object(i,'verify_package',return_value={}),patch.object(i,'run') as run,contextlib.redirect_stdout(io.StringIO()):
            i.main()
        run.assert_not_called()

    @unittest.skipUnless(os.environ.get('HACKIOS_TEST_ADB') or (P/'tools/platform-tools/adb.exe').is_file(), 'Set HACKIOS_TEST_ADB to test an external adb binary')
    def test_real_shipped_adb_starts_from_no_daemon_with_remote_environment(self):
        with socket.socket() as reserved:
            reserved.bind(('127.0.0.1',0));port=reserved.getsockname()[1]
        p=i.Phone();p.adb=Path(os.environ.get('HACKIOS_TEST_ADB',str(p.adb)));calls=[];started=False;observations=[]
        # Both inherited remote settings are intentionally unusable. The explicit
        # hostless -L must override them without changing the user's environment.
        env=dict(os.environ,ADB_SERVER_SOCKET='tcp:192.0.2.1:1',ANDROID_ADB_SERVER_ADDRESS='192.0.2.1',
            ANDROID_ADB_SERVER_PORT='1',ADB_MDNS_AUTO_CONNECT='')
        def local_run(args,timeout=30,binary=False):
            nonlocal started
            args=list(map(str,args));calls.append(args[1:])
            if 'start-server' in args:
                self.assertEqual(args[1:],['-L','tcp:5037','start-server'])
                args[2]='tcp:'+str(port)
                args[-1:-1]=['--one-device','HACKIOS-NO-PHYSICAL-DEVICE-COLD-TEST']
            else:
                self.assertEqual(args[1:],['-H','127.0.0.1','-P','5037','devices'])
                args[args.index('-P')+1]=str(port)
            result=subprocess.run(args,capture_output=True,timeout=timeout,env=env)
            observations.append({'arguments':args[1:],'returncode':result.returncode,
                'stdout':result.stdout.decode(errors='replace'),'stderr':result.stderr.decode(errors='replace')})
            self.assertEqual(result.returncode,0,result.stderr.decode(errors='replace'))
            if 'start-server' in args:started=True
            return (result.stdout+result.stderr).decode(errors='replace')
        try:
            with patch.object(i,'run',side_effect=local_run):
                self.assertEqual(p.adb_devices(),[]);self.assertEqual(p.adb_devices(),[])
            self.assertEqual(sum('start-server' in c for c in calls),1)
        finally:
            if started:
                result=subprocess.run([str(p.adb),'-H','127.0.0.1','-P',str(port),'kill-server'],capture_output=True,timeout=15)
                self.assertEqual(result.returncode,0,result.stderr)

if __name__=='__main__':unittest.main()

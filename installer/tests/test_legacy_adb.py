"""Real BusyBox ash execution, binary fixtures and interrupted legacy ADB frames.

All devices are simulated; file writes stay in TemporaryDirectory.
"""
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
import contextlib,io,os,re,secrets,shlex,shutil,subprocess,sys,tempfile,unittest

W=Path(__file__).resolve().parent;P=W.parent
sys.path.insert(0,str(P))
import installer as i
BB=Path(os.environ.get('HACKIOS_TEST_BUSYBOX','/nonexistent/busybox'))

@unittest.skipUnless(BB.is_file(), 'Set HACKIOS_TEST_BUSYBOX to test real shell framing')
class LegacyADBTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=W);self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name);self.events=[]
        logger=patch.object(i,'record_command',side_effect=self.events.append);logger.start();self.addCleanup(logger.stop)

    def execute(self,cmd,token=None):
        token=token or secrets.token_hex(16)
        script=self.sandbox_script(i.legacy_shell_script(cmd,token))
        # Match the legacy adb daemon's raw shell command, not shell_v2.
        result=subprocess.run([str(BB),'sh','-c','sh -c '+shlex.quote(script)],cwd=self.root,
            env=dict(os.environ,TMPDIR=str(self.root)),capture_output=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(result.stderr,b'')
        self.assertFalse(list(self.root.glob('hackios-adb-*')),'Remote stderr temporary files leaked')
        return result.stdout,token

    def sandbox_script(self,script,ram=None,android=None):
        # Map device temp paths into test-owned directories; never use a host
        # /tmp merely because BusyBox happens to provide one.
        ram=ram or self.root
        android=android or self.root/'android-tmp'
        return re.sub(r'/data/local/tmp|/tmp\b',lambda m:(android if m[0].startswith('/data/') else ram).relative_to(self.root).as_posix(),script)

    def test_recovery_ram_precedes_data_tmpdir_and_cleans_open_stderr(self):
        ram=self.root/'ram';ram.mkdir();data=self.root/'data';data.mkdir()
        token=secrets.token_hex(16)
        command='printf "%s\\n" "$_hackios_dir"; test -f "$_hackios_dir/stderr"; printf warning >&2'
        script=self.sandbox_script(i.legacy_shell_script(command,token),ram=ram)
        result=subprocess.run([str(BB),'sh','-c',script],cwd=self.root,
            env=dict(os.environ,TMPDIR='data'),capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(i.decode_legacy_shell(result.stdout,token),'ram/hackios-adb-'+token+'\nwarning')
        self.assertEqual(list(ram.iterdir()),[]);self.assertEqual(list(data.iterdir()),[])

    def test_android_without_ram_tmp_retains_tmpdir_compatibility(self):
        data=self.root/'data';data.mkdir();token=secrets.token_hex(16)
        script=self.sandbox_script(i.legacy_shell_script('printf "%s" "$_hackios_tmp"',token),ram=self.root/'absent')
        result=subprocess.run([str(BB),'sh','-c',script],cwd=self.root,
            env=dict(os.environ,TMPDIR='data'),capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(i.decode_legacy_shell(result.stdout,token),'data')
        self.assertEqual(list(data.iterdir()),[])

    def test_android_without_tmpdir_uses_local_tmp_fallback(self):
        android=self.root/'android';android.mkdir();token=secrets.token_hex(16)
        script=self.sandbox_script(i.legacy_shell_script('printf "%s" "$_hackios_tmp"',token),
            ram=self.root/'absent',android=android)
        result=subprocess.run([str(BB),'sh','-c',script],cwd=self.root,
            env=dict(os.environ,TMPDIR=''),capture_output=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertEqual(i.decode_legacy_shell(result.stdout,token),'android')
        self.assertEqual(list(android.iterdir()),[])

    def test_binary_all_bytes_and_dd_stderr_remain_separate(self):
        data=bytes(range(256))*16384
        (self.root/'data.bin').write_bytes(data)
        raw,token=self.execute('dd if=data.bin bs=4096 count=1024')
        self.assertEqual(i.decode_legacy_shell(raw,token,True),data)
        self.assertIn('records',self.events[-1]['stderr'])
        self.assertEqual(self.events[-1]['returncode'],0)
        self.assertIsNone(self.events[-1]['stdout'])

    def test_nonzero_remote_status_is_enforced_despite_host_success(self):
        raw,token=self.execute("printf private_binary; printf 'dd: input/output error\\n' >&2; exit 7")
        with self.assertRaisesRegex(RuntimeError,'退出码 7') as caught:i.decode_legacy_shell(raw,token,True)
        self.assertIn('input/output error',str(caught.exception))
        self.assertNotIn('private_binary',str(caught.exception))

    def test_empty_output_and_no_final_newline_and_text_stderr(self):
        for cmd,out in [('true',''),('printf yes','yes'),('printf yes; printf warning >&2','yeswarning')]:
            with self.subTest(cmd=cmd):
                raw,token=self.execute(cmd)
                self.assertEqual(i.decode_legacy_shell(raw,token),out)

    def test_quotes_subshell_pipe_exit_and_marker_like_binary_payload(self):
        token='d'*32
        data=b"quotes ' \" $() ` \\ \x00\r\n\xff\nHACKIOS-END-"+token.encode()+b':1:0\n'
        (self.root/'quoted file.bin').write_bytes(data)
        raw,token=self.execute("cat 'quoted file.bin'",token)
        self.assertEqual(i.decode_legacy_shell(raw,token,True),data)
        raw,token=self.execute('set -o pipefail; false | cat')
        with self.assertRaisesRegex(RuntimeError,'退出码 1'):i.decode_legacy_shell(raw,token)

    def test_missing_footer_prefix_or_truncated_error_is_rejected(self):
        raw,token=self.execute('printf data; printf err >&2')
        for bad in (b'',raw[:-1],raw[:-12],raw[1:],b'banner\n'+raw,raw+b'garbage',raw.replace(b':0:3\n',b':0:9999\n')):
            with self.subTest(bad=bad[-40:]),self.assertRaises(RuntimeError):i.decode_legacy_shell(bad,token,True)

    def test_wrong_nonce_and_impossible_exit_code_are_rejected(self):
        raw,token=self.execute('printf data')
        with self.assertRaises(RuntimeError):i.decode_legacy_shell(raw,'e'*32,True)
        with self.assertRaises(RuntimeError):i.decode_legacy_shell(raw.replace(b':0:0\n',b':256:0\n'),token,True)

    def legacy_run(self,args,timeout,binary):
        self.assertTrue(binary)
        self.assertEqual(args[-2],'exec-out')
        script=shlex.split(args[-1]);self.assertEqual(len(script),3)
        self.assertEqual(script[:2],['sh','-c'])
        local_script=self.sandbox_script(shlex.split(args[-1])[2])
        result=subprocess.run([str(BB),'sh','-c',local_script],cwd=self.root,
            env=dict(os.environ,TMPDIR=str(self.root)),capture_output=True,timeout=timeout)
        self.assertEqual(result.returncode,0,result.stderr)
        return result.stdout

    def test_phone_read_uses_actual_legacy_shell_for_chunked_binary_reads(self):
        data=bytes(range(256))*(16384+16)
        (self.root/'partition.bin').write_bytes(data)
        phone=i.Phone();phone.serial='LEGACY'
        original=i.legacy_shell_script
        def local_script(cmd,token):return original(cmd.replace('/dev/block/by-name/boot','partition.bin'),token)
        with patch.object(i,'run',side_effect=self.legacy_run),patch.object(i,'legacy_shell_script',side_effect=local_script):
            self.assertEqual(phone.read('/dev/block/by-name/boot',0,len(data)),data)
        self.assertEqual([e['actual_bytes'] for e in self.events if e['event']=='block_read'],[4*1024**2,4096])

    def test_remote_read_error_is_not_retried_or_used_as_data(self):
        phone=i.Phone();phone.serial='LEGACY';original=i.legacy_shell_script
        with patch.object(i,'run',side_effect=self.legacy_run) as transport,patch.object(i,'legacy_shell_script',side_effect=lambda cmd,t:original('dd if=missing-file bs=4096 count=1',t)):
            with self.assertRaisesRegex(RuntimeError,'退出码'):phone.read('/dev/block/by-name/boot',0,4096)
        self.assertEqual(transport.call_count,1)

    def test_old_recovery_passes_without_any_features_query(self):
        commands=[]
        def shell(cmd):
            commands.append(cmd)
            return {'getprop ro.product.device':'raphael','id':'uid=0(root)',
                    'getprop ro.twrp.version':'3.3.1','cat /sys/class/block/sda/queue/logical_block_size':'4096',
                    'cat /proc/mounts':''}[cmd]
        phone=SimpleNamespace(shell=shell,adb_command=lambda *a:self.fail('Legacy recovery must not require features'))
        i.Phone._check_recovery(phone)
        self.assertFalse(any('features' in c for c in commands))

    def test_recovery_unmount_failure_remains_fatal_and_uses_normal_umount(self):
        calls=[]
        def shell(cmd,*args):
            calls.append(cmd)
            if cmd=='getprop ro.product.device':return 'raphael'
            if cmd=='id':return 'uid=0(root)'
            if cmd=='getprop ro.twrp.version':return '3.3.1'
            if cmd=='cat /sys/class/block/sda/queue/logical_block_size':return '4096'
            if cmd=='cat /proc/mounts':return '/dev/block/sda31 /data ext4 rw 0 0\n'
            if cmd.startswith('twrp unmount '):return 'Failed to unmount (Device or resource busy)'
            if cmd=='umount /data':raise RuntimeError('umount: Device or resource busy')
            self.fail('Unexpected command: '+cmd)
        with patch.object(i.time,'sleep'),self.assertRaisesRegex(RuntimeError,'数据仍挂载'):
            i.Phone._check_recovery(SimpleNamespace(shell=shell))
        self.assertEqual(calls.count('umount /data'),3)
        self.assertFalse(any('|| true' in c or '/dev/null' in c or 'umount -' in c for c in calls))

    def test_recovery_unmount_fallback_rechecks_mount_table(self):
        mounted=True;calls=[]
        def shell(cmd,*args):
            nonlocal mounted
            calls.append(cmd)
            if cmd=='getprop ro.product.device':return 'raphael'
            if cmd=='id':return 'uid=0(root)'
            if cmd=='getprop ro.twrp.version':return '3.3.1'
            if cmd=='cat /sys/class/block/sda/queue/logical_block_size':return '4096'
            if cmd=='cat /proc/mounts':return '/dev/block/sda31 /data ext4 rw 0 0\n' if mounted else ''
            if cmd.startswith('twrp unmount '):return 'Failed to unmount (Device or resource busy)'
            if cmd=='umount /data':mounted=False;return ''
            self.fail('Unexpected command: '+cmd)
        with patch.object(i.time,'sleep'):i.Phone._check_recovery(SimpleNamespace(shell=shell))
        self.assertFalse(mounted);self.assertEqual(calls.count('umount /data'),1)
        self.assertEqual(calls[-1],'cat /proc/mounts')

    def test_write_command_is_executed_once_when_reply_is_lost(self):
        phone=i.Phone();phone.serial='LEGACY';calls=[]
        def lost_reply(args,timeout,binary):
            calls.append(args)
            raw=self.legacy_run(args,timeout,binary)
            return raw[:-10]
        with patch.object(i,'run',side_effect=lost_reply),self.assertRaisesRegex(RuntimeError,'不完整'):
            phone.shell("printf executed >> write-count.txt; printf done")
        self.assertEqual(len(calls),1)
        self.assertEqual((self.root/'write-count.txt').read_bytes(),b'executed')

    def test_write_failure_never_claims_success_or_retries(self):
        phone=i.Phone();phone.serial='LEGACY'
        with patch.object(i,'run',side_effect=self.legacy_run) as transport,self.assertRaisesRegex(RuntimeError,'退出码'):
            phone.shell('dd if=missing-file of=output.bin bs=4096 count=1 conv=notrunc && sync')
        self.assertEqual(transport.call_count,1)

    def write_fixture(self):
        phone=i.Phone();phone.serial='LEGACY';phone.session=self.root
        phone.allow(i.RECOVERY,4096,8192)
        (self.root/'partition.bin').write_bytes(b'A'*12288)
        def upload(*args,**kwargs):
            self.assertEqual(args[0],'push')
            shutil.copyfile(args[1],self.root/'remote-chunk.bin')
            return 'OK'
        phone.adb_command=upload
        original=i.legacy_shell_script
        def local_script(cmd,token):
            return original(cmd.replace(i.RECOVERY,'partition.bin').replace('/tmp/hackios-oneclick-chunk.bin','remote-chunk.bin'),token)
        return phone,local_script

    def test_actual_phone_write_push_hash_dd_and_readback_on_legacy_transport(self):
        phone,local_script=self.write_fixture();data=bytes(range(256))*16
        with patch.object(i,'run',side_effect=self.legacy_run),patch.object(i,'legacy_shell_script',side_effect=local_script):
            phone.write(i.RECOVERY,4096,data)
        self.assertEqual((self.root/'partition.bin').read_bytes(),b'A'*4096+data+b'A'*4096)
        self.assertFalse((self.root/'transfer-chunk.bin').exists())
        self.assertTrue(any(e.get('event')=='block_read' for e in self.events))

    def test_actual_phone_write_failure_stops_before_readback(self):
        phone,local_script=self.write_fixture()
        def bad_script(cmd,token):return local_script(cmd.replace('of='+i.RECOVERY,'of=missing-directory/partition'),token)
        with patch.object(i,'run',side_effect=self.legacy_run),patch.object(i,'legacy_shell_script',side_effect=bad_script):
            with self.assertRaisesRegex(RuntimeError,'退出码'):phone.write(i.RECOVERY,4096,b'B'*4096)
        self.assertFalse(any(e.get('event')=='block_read' for e in self.events))
        self.assertEqual((self.root/'partition.bin').read_bytes(),b'A'*12288)
        self.assertFalse((self.root/'transfer-chunk.bin').exists())

    def test_actual_phone_write_lost_completion_does_not_replay(self):
        phone,local_script=self.write_fixture();mutations=[]
        def dropped(args,timeout,binary):
            result=self.legacy_run(args,timeout,binary)
            if ' of=' in shlex.split(args[-1])[-1]:
                mutations.append(args);return result[:-10]
            return result
        with patch.object(i,'run',side_effect=dropped),patch.object(i,'legacy_shell_script',side_effect=local_script):
            with self.assertRaisesRegex(RuntimeError,'不完整'):phone.write(i.RECOVERY,4096,b'B'*4096)
        self.assertEqual(len(mutations),1)
        self.assertEqual((self.root/'partition.bin').read_bytes(),b'A'*4096+b'B'*4096+b'A'*4096)
        self.assertFalse((self.root/'transfer-chunk.bin').exists())

    def test_stderr_temporary_file_failure_cannot_look_like_success(self):
        token='f'*32
        script=self.sandbox_script(i.legacy_shell_script('printf MUST_NOT_RUN',token))
        # Directory collision before the user command: no accepted frame.
        (self.root/('hackios-adb-'+token)).mkdir()
        result=subprocess.run([str(BB),'sh','-c',script],cwd=self.root,
            env=dict(os.environ,TMPDIR=str(self.root)),capture_output=True,timeout=10)
        self.assertNotEqual(result.returncode,0);self.assertNotIn(b'MUST_NOT_RUN',result.stdout)
        with self.assertRaises(RuntimeError):i.decode_legacy_shell(result.stdout,token)

if __name__=='__main__':unittest.main()

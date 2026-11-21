"""Recovery startup USB restart cases. No phone access or partition mutation."""
from pathlib import Path
from unittest.mock import Mock,patch
import contextlib,io,sys,unittest
W=Path(__file__).resolve().parent;sys.path.insert(0,str(W.parent))
import installer as i
BOOT='00000000-0000-0000-0000-000000000001'

def response(version='3.7.0',product='raphael',completed='',adbd='running',config='adb',state='adb',pid='651',boot=BOOT):
    return '\n'.join([product,version,completed,adbd,config,state,pid,boot])+'\n'

class RecoveryReadyTests(unittest.TestCase):
    def setUp(self):
        self.tick=0;self.events=[];self.probes=[]
        self.p=i.Phone();self.p.serial='SYNTHETIC';self.p.adb_devices=lambda:[['SYNTHETIC','recovery']]
        for obj,name,replacement in [(i.time,'monotonic',lambda:self.tick),(i.time,'sleep',self.advance),(i,'record_command',self.events.append)]:
            patcher=patch.object(obj,name,replacement);patcher.start();self.addCleanup(patcher.stop)
        redirect=contextlib.redirect_stdout(io.StringIO());redirect.__enter__();self.addCleanup(redirect.__exit__,None,None,None)
        self.p.check_recovery=Mock(side_effect=AssertionError('Startup wait must not unmount'))
        self.p.write=Mock(side_effect=AssertionError('Startup wait must not write'))
        self.p.adb_command=Mock(side_effect=AssertionError('Startup wait must not reboot or push'))

    def advance(self,seconds):self.tick+=seconds

    def shell(self,result):
        def execute(cmd,timeout=10):
            self.probes.append(cmd);self.assertEqual(cmd,i.RECOVERY_READY_PROBE)
            if isinstance(result,BaseException):raise result
            return result
        return execute

    def test_startup_disconnect_and_mtp_switch_recover_before_any_mutation(self):
        # Original failure: version was readable, then adbd died mid-response,
        # disappeared, and returned in the same boot with MTP+ADB and a new PID.
        def devices():return [] if self.tick==2 else [['SYNTHETIC','recovery']]
        self.p.adb_devices=devices
        def execute(cmd,timeout=10):
            self.probes.append(cmd);self.assertEqual(cmd,i.RECOVERY_READY_PROBE)
            if self.tick==1:raise RuntimeError('ADB 命令返回不完整（完成标记缺失）')
            return response() if self.tick==0 else response(config='mtp,adb',state='mtp,adb',pid='700')
        self.p.shell=execute;self.p.wait_recovery_ready()
        self.assertGreaterEqual(self.tick,8);self.assertEqual(self.p.recovery_boot_id,BOOT)
        self.assertEqual(len([e for e in self.events if e['event']=='recovery_probe_interrupted']),1)
        self.p.write.assert_not_called();self.p.check_recovery.assert_not_called();self.p.adb_command.assert_not_called()

    def test_usb_state_must_finish_transition(self):
        self.p.shell=lambda cmd,timeout=10:response(config='mtp,adb',state='adb' if self.tick<3 else 'mtp,adb')
        self.p.wait_recovery_ready();self.assertGreaterEqual(self.tick,8)

    def test_pid_change_resets_stable_timer_even_without_missing_device(self):
        self.p.shell=lambda cmd,timeout=10:response(pid='651' if self.tick<4 else '700')
        self.p.wait_recovery_ready();self.assertGreaterEqual(self.tick,9)

    def test_optional_old_recovery_properties_may_be_empty(self):
        self.p.shell=self.shell(response(adbd='',config='',state='',pid=''))
        self.p.wait_recovery_ready();self.assertEqual(self.tick,5)

    def test_wrong_phone_is_rejected_without_probing(self):
        self.p.adb_devices=lambda:[['OTHER','recovery']];self.p.shell=Mock()
        with self.assertRaisesRegex(RuntimeError,'另一台手机'):self.p.wait_recovery_ready()
        self.p.shell.assert_not_called()

    def test_android_boot_does_not_pass_recovery_wait(self):
        self.p.shell=self.shell(response(version='',completed='1'))
        with self.assertRaisesRegex(RuntimeError,'手机进入了 Android'):self.p.wait_recovery_ready()
        self.assertEqual(self.tick,0)

    def test_wrong_model_is_rejected_immediately(self):
        self.p.shell=self.shell(response(product='OTHER'))
        with self.assertRaisesRegex(RuntimeError,'机型不匹配'):self.p.wait_recovery_ready()
        self.assertEqual(self.tick,0)

    def test_reconnect_to_different_kernel_boot_stops(self):
        self.p.recovery_boot_id=BOOT
        self.p.shell=self.shell(response(boot='00000000-0000-0000-0000-000000000002'))
        with self.assertRaisesRegex(RuntimeError,'手机已重新启动'):self.p.wait_recovery_ready()

    def test_authorized_new_temporary_boot_accepts_new_kernel_id(self):
        self.p.recovery_boot_id='00000000-0000-0000-0000-000000000002';self.p.shell=self.shell(response())
        self.p.wait_recovery_ready(new_boot=True);self.assertEqual(self.p.recovery_boot_id,BOOT)

    def test_bounded_wait_preserves_last_error_and_never_mutates(self):
        self.p.shell=self.shell(RuntimeError('frame completion lost'))
        with self.assertRaisesRegex(RuntimeError,'超时.*frame completion lost'):self.p.wait_recovery_ready(timeout=4)
        self.assertEqual(len(self.probes),4);self.p.write.assert_not_called();self.p.adb_command.assert_not_called()

    def test_incomplete_probe_fields_never_pass(self):
        self.p.shell=self.shell('raphael\n3.7.0\n')
        with self.assertRaisesRegex(RuntimeError,'字段不完整'):self.p.wait_recovery_ready(timeout=4)

    def test_connect_waits_before_checking_or_preparing_existing_recovery(self):
        calls=[];self.p.adb_devices=lambda:[['SYNTHETIC','recovery']];self.p.fastboot_devices=lambda:[]
        self.p.wait_recovery_ready=lambda:calls.append('stable')
        self.p.check_recovery=lambda:calls.append('check')
        self.p.prepare_android_recovery=lambda:calls.append('prepare')
        self.p.shell=Mock(side_effect=AssertionError('No queries before stable wait'))
        self.p.connect();self.assertEqual(calls,['stable','check','prepare'])

    def test_missing_frame_records_metadata_without_exposing_binary(self):
        token='a'*32;raw=('HACKIOS-BEGIN-'+token+'\n').encode()+b'PRIVATE_BINARY'
        with self.assertRaisesRegex(RuntimeError,'完成标记缺失'):i.decode_legacy_shell(raw,token,True)
        event=self.events[-1];self.assertEqual(event['event'],'remote_shell_incomplete')
        self.assertTrue(event['begin_marker_present']);self.assertFalse(event['end_marker_present'])
        self.assertEqual(event['received_bytes'],len(raw));self.assertNotIn('PRIVATE_BINARY',str(event))

if __name__=='__main__':unittest.main()

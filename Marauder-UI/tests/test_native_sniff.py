import struct,unittest,tempfile,queue
from concurrent.futures import Future
from types import SimpleNamespace
from unittest.mock import patch
from native_sniff import parse_host_only,MODES
from host_connection import Op,Frame,Channel,Kind
from server import Bridge
from test_server import FakeSerial
class NativeSniffTests(unittest.TestCase):
 def test_all_native_modes(self):
  for cmd,i in MODES.items():self.assertEqual(parse_host_only(cmd+' --host-only'),bytes([i,0,0]))
 def test_original_parameters_preserved(self):
  self.assertEqual(parse_host_only('sniffpmkid -c 36 -d -l --host-only'),bytes([5,7,36]))
  self.assertEqual(parse_host_only('sniffpmkid --host-only -l -c 6'),bytes([5,5,6]))
  self.assertIsNone(parse_host_only('sniffpmkid -c 6 -l'))
 def test_invalid_flags_never_become_radio_commands(self):
  for c in ['sniffprobe --host-only=true','help --host-only','sniffbt --host-only','sniffprobe -d --host-only','sniffprobe --host-only --host-only','sniffpmkid -c --host-only','sniffpmkid -c 256 --host-only','sniffpmkid -c 0 --host-only','sniffpmkid -c -1 --host-only','sniffpmkid -d -d --host-only','sniffraw -serial --host-only']:
   with self.subTest(c=c),self.assertRaises(ValueError):parse_host_only(c)
 def setUp(self):
  self.b=Bridge();self.b.running=False;self.b.thread.join(1);self.b.ser=FakeSerial();self.b.update(verified=True,capabilities=['capture-host-only-v1','native-sniff-stream-v1'])
 def tearDown(self):
  self.b.host_client=None;self.b.close()
 def test_new_command_does_not_speculate_on_old_firmware(self):
  calls=[];self.b.host_client=SimpleNamespace(request=lambda *a,**k:calls.append(a));self.b.update(capabilities=['capture-host-only-v1'])
  with self.assertRaisesRegex(ValueError,'更新固件'):self.b.console('sniffprobe --host-only')
  self.assertEqual(calls,[]);self.assertEqual(self.b.ser.commands,[])
 def test_no_flag_uses_existing_cli(self):
  calls=[];f=Future();f.set_result(b'');self.b.host_client=SimpleNamespace(cli=lambda c:(calls.append(c),f)[1]);self.b.host_wait=lambda f:f.result()
  self.b.console('sniffpmkid -c 6 -l');self.assertEqual(calls,['sniffpmkid -c 6 -l'])
 def test_typed_single_mutation_and_early_frame_staging(self):
  calls=[]
  def request(op,payload,**kw):
   calls.append((op,payload,kw))
   record=struct.pack('<IIIQHHbB',31,0,2,12345,24,24,-51,6)+bytes(24)
   self.b.host_event(Frame(Channel.CAPTURE,Kind.DATA,Op.CAPTURE_RECORD,1,payload=record))
   f=Future();f.set_result(struct.pack('<IIHB',31,2,2324,1));return f
  self.b.host_client=SimpleNamespace(request=request);self.b.host_wait=lambda f:f.result()
  result=self.b.console('sniffpmkid -c 6 --host-only')
  self.assertEqual(calls,[(Op.SNIFF_START,bytes([5,1,6]),{'mutation':True})]);self.assertTrue(result['host_only'])
  self.b.capture_sink.close();self.assertEqual(self.b.capture_sink.snapshot()['received'],1)
 def test_start_failure_is_not_retried_or_converted_to_legacy(self):
  calls=[]
  def request(*a,**kw):calls.append(a);f=Future();f.set_exception(TimeoutError('completion unknown'));return f
  self.b.host_client=SimpleNamespace(request=request);self.b.host_wait=lambda f:f.result()
  with self.assertRaises(TimeoutError):self.b.console('sniffdeauth --host-only')
  self.assertEqual(len(calls),1);self.assertIsNone(self.b.pending_capture);self.assertEqual(self.b.ser.commands,[])
 def test_removed_capture_command_never_reaches_device(self):
  for cmd in ['capture start','capture stop','capture status']:
   with self.assertRaises(ValueError):self.b.console(cmd)
  self.assertEqual(self.b.ser.commands,[])

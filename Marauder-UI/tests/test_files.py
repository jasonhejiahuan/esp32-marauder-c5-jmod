import base64,binascii,json,time,unittest
from unittest.mock import patch
from test_server import FakeSerial
from server import Bridge,Model
class FileSerial(FakeSerial):
 def __init__(self,bad=False):super().__init__();self.bad=bad
 def write(self,data):
  self.commands.append(data.decode().strip());a=data.decode().split();tx=a[2];op=a[1];raw=b'\x00pcap\xff\r\n'
  if op=='read':result=dict(kind='end',ok=True,offset=int(a[4]),size=len(raw),data=base64.b64encode(raw).decode(),crc32=binascii.crc32(raw)+(1 if self.bad else 0))
  elif op=='list':
   self.buffer+=('@FILE:'+json.dumps(dict(tx=tx,kind='entry',name='中文.pcap'.encode().hex(),directory=False,size=8))+'\n').encode();result=dict(kind='end',ok=True,next=-1,total=2097152,used=4096)
  else:result=dict(kind='end',ok=True,size=len(raw))
  self.buffer+=('@FILE:'+json.dumps(dict(tx='unrelated',kind='end',ok=False))+'\n@FILE:'+json.dumps(dict(tx=tx,**result))+'\n').encode();return len(data)
class FilesTests(unittest.TestCase):
 def setUp(self):
  self.b=Bridge();self.b.running=False;self.b.thread.join(1);self.b.ser=FileSerial();self.b.update(verified=True,connected=True,mode='idle',manual=False,capabilities=['files-v1'])
 def tearDown(self):self.b.close()
 def test_binary_transfer_checks_crc_and_keeps_payload_off_sse(self):
  r=self.b.files('read','/中文.pcap');self.assertEqual(base64.b64decode(r['data']),b'\x00pcap\xff\r\n');self.assertEqual(list(self.b.model.logs),[])
  self.b.ser.bad=True
  with self.assertRaisesRegex(ValueError,'校验'):self.b.files('read','/中文.pcap')
 def test_only_read_operations_retry_a_lost_response(self):
  with patch.object(self.b,'file_exchange',side_effect=[TimeoutError(),([],{'ok':True})]) as exchange:
   self.assertTrue(self.b.file_request('read','/x')[1]['ok']);self.assertEqual(exchange.call_count,2)
  with patch.object(self.b,'file_exchange',side_effect=TimeoutError()) as exchange:
   with self.assertRaises(TimeoutError):self.b.file_request('rename','/x')
   self.assertEqual(exchange.call_count,1)
 def test_listing_unicode_and_transaction_isolation(self):
  r=self.b.files('list');self.assertEqual(r['entries'][0]['path'],'/中文.pcap');self.assertEqual(r['next'],-1)
 def test_paths_and_sizes_rejected_before_usb(self):
  for path in ['/../x','/a/./b','/a//b','/a\nreboot','/a\\b','/a/','x','/'+'字'*60]:
   with self.assertRaises(ValueError):self.b.files('stat',path)
  for offset,length in [(-1,1024),(True,1),(0,1025),(0,0)]:
   with self.assertRaises(ValueError):self.b.files('read','/x',offset=offset,length=length)
  self.assertEqual(self.b.ser.commands,[])
 def test_busy_old_firmware_and_unknown_operations_never_write(self):
  self.b.update(mode='scancombined')
  with self.assertRaises(ValueError):self.b.files('list')
  self.b.update(mode='idle',capabilities=[])
  with self.assertRaises(ValueError):self.b.files('list')
  self.b.update(capabilities=['files-v1'])
  with self.assertRaises(ValueError):self.b.files('erase')
  self.assertEqual(self.b.ser.commands,[])
 def test_delete_needs_capability_confirmation_and_trash_scope(self):
  for op,path in [('delete','/.trash/a'),('emptytrash','/.trash')]:
   with self.assertRaisesRegex(ValueError,'不支持'):self.b.files(op,path,confirm=True)
  self.b.update(capabilities=['files-v1','files-delete-v1'])
  for confirm in [False,None,1,'true']:
   with self.assertRaisesRegex(ValueError,'确认'):self.b.files('delete','/.trash/a',confirm=confirm)
  for op,path in [('delete','/'),('delete','/.trash'),('delete','/x'),('delete','/.trashable/x'),('emptytrash','/'),('emptytrash','/.trash/a')]:
   with self.assertRaises(ValueError):self.b.files(op,path,confirm=True)
  self.assertEqual(self.b.ser.commands,[])
 def test_delete_and_empty_trash_legacy_dispatch_once(self):
  self.b.update(capabilities=['files-v1','files-delete-v1'])
  for op,path in [('delete','/.trash/中文.pcap'),('emptytrash','/.trash')]:
   self.assertTrue(self.b.files(op,path,confirm=True)['ok'])
   command=self.b.ser.commands[-1].split();self.assertEqual(command[1],op);self.assertEqual(bytes.fromhex(command[3]).decode(),path)
  for op in ['delete','emptytrash']:
   with patch.object(self.b,'file_exchange',side_effect=TimeoutError()) as exchange:
    with self.assertRaisesRegex(TimeoutError,'不会自动重试'):self.b.files(op,'/.trash/x' if op=='delete' else '/.trash',confirm=True)
    self.assertEqual(exchange.call_count,1)
 def test_delete_partial_error_is_explicit_and_never_retried(self):
  self.b.update(capabilities=['files-v1','files-delete-v1'])
  for code in ['delete_partial','delete_limit_partial']:
   with patch.object(self.b,'file_exchange',side_effect=ValueError('文件操作失败：'+code)) as exchange:
    with self.assertRaisesRegex(ValueError,'部分内容已删除'):self.b.files('delete','/.trash/x',confirm=True)
    self.assertEqual(exchange.call_count,1)
 def test_destructive_operations_remain_excluded_while_busy(self):
  self.b.update(capabilities=['files-v1','files-delete-v1'],mode='scancombined')
  with self.assertRaises(ValueError):self.b.files('emptytrash','/.trash',confirm=True)
  self.b.update(mode='idle',manual=True)
  with self.assertRaises(ValueError):self.b.files('delete','/.trash/x',confirm=True)
  self.assertEqual(self.b.ser.commands,[])
 def test_binary_deletion_is_typed_and_timeout_indeterminate(self):
  from transport import HostConnectionClient,IndeterminateError
  from host_connection import Reader,Op
  from test_host_connection import PartialSerial
  c=HostConnectionClient(77,lambda:None)
  for op,path,number in [('delete','/.trash/中文.pcap',8),('emptytrash','/.trash',9)]:
   future=c.file_operation(op,path);frame=c.tx[-1][0];pending=c.pending[frame.request]
   self.assertEqual(frame.op,Op.FILE_OP);r=Reader(frame.payload);self.assertEqual(r.unpack('B'),(number,));self.assertEqual(r.string(),path)
   self.assertTrue(pending.mutation);self.assertGreater(pending.deadline-pending.started,13)
   pending.deadline=0;c.pump(PartialSerial());self.assertRaises(IndeterminateError,future.result)
  c.close()
 def test_expired_queued_deletion_never_reaches_the_endpoint(self):
  self.b.update(capabilities=['files-v1','files-delete-v1'])
  for op,path in [('delete','/.trash/x'),('emptytrash','/.trash')]:
   job=dict(kw=dict(operation=op,path=path,confirm=True),expires=time.monotonic()-1)
   with self.assertRaisesRegex(TimeoutError,'未执行'):self.b.file_job(job)
  self.assertEqual(self.b.ser.commands,[])
  self.assertTrue(self.b.file_job(dict(kw=dict(operation='delete',path='/.trash/x',confirm=True),expires=time.monotonic()+10))['ok'])
  self.assertEqual(len(self.b.ser.commands),1)
class BlePresenceTests(unittest.TestCase):
 def test_only_healthy_active_scan_time_expires_devices(self):
  m=Model();status=lambda active,drop=0:m.log('@BLE:'+json.dumps(dict(kind='status',active=active,dropped=drop,tableFull=0)))
  with patch('server.time.monotonic',return_value=0):status(True)
  m.log('@BLE:'+json.dumps(dict(kind='device',mac='aa:bb:cc:dd:ee:ff',address_type=1,name='',rssi=-60,age_ms=0)))
  d=next(iter(m.ble.values()))
  for t in range(2,20,2):
   with patch('server.time.monotonic',return_value=t):status(True)
  self.assertTrue(d['online'])
  with patch('server.time.monotonic',return_value=20):status(True,1)
  self.assertTrue(d['online'])
  with patch('server.time.monotonic',return_value=100):status(True,1)
  self.assertTrue(d['online'])
  with patch('server.time.monotonic',return_value=102):status(False,1)
  self.assertFalse(d['online']);clock=m.ble_clock
  with patch('server.time.monotonic',return_value=1000):status(False,1)
  self.assertEqual(clock,m.ble_clock)
  m.log('@BLE:'+json.dumps(dict(kind='device',mac='aa:bb:cc:dd:ee:ff',address_type=1,name='',rssi=-60,age_ms=0)))
  self.assertTrue(next(iter(m.ble.values()))['online'])

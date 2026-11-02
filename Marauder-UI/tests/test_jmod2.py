import json,struct,time,unittest
from concurrent.futures import Future
from types import SimpleNamespace
from unittest.mock import patch
from server import Bridge,Model
from host_connection import Op,Reader,Channel,Frame,Kind
from test_server import FakeSerial

class Jmod2Tests(unittest.TestCase):
 def setUp(self):
  self.b=Bridge();self.b.running=False;self.b.thread.join(1);self.b.ser=FakeSerial();self.b.update(verified=True,connected=True,mode='idle',manual=False,telemetry_enabled=True,capabilities=['selection-v1','capture-host-only-v1','device-metrics-v1']);self.b.telemetry_clients={};self.b.next_metrics=0
 def tearDown(self):
  self.b.host_client=None;self.b.close()
 def client(self,reply):
  calls=[]
  def request(op,payload=b'',**kw):
   calls.append((op,payload,kw));f=Future()
   try:f.set_result(reply(op,payload,kw))
   except Exception as e:f.set_exception(e)
   return f
  self.b.host_client=SimpleNamespace(request=request);return calls
 def test_visible_manual_downshifts_and_other_pages_stop(self):
  calls=self.client(lambda *args:b'')
  self.b.host_metrics=lambda f:setattr(self.b,'metrics_pending',0)
  b=self.b;b.telemetry(client='overview01',view='overview');b.update(manual=True,mode='console')
  self.assertEqual(b.metrics_interval(time.monotonic()),8)
  b.next_metrics=0;b.poll_metrics();self.assertEqual(calls[0][0],Op.TELEMETRY)
  for _ in range(10):b.poll_metrics()
  self.assertEqual(len(calls),1)
  b.telemetry(client='overview01',view='background');b.next_metrics=0
  for _ in range(10):b.poll_metrics()
  self.assertEqual(len(calls),1);self.assertEqual(b.metrics_interval(time.monotonic()),0)
 def test_async_wait_keeps_telemetry_and_rx_moving(self):
  f=Future();calls=[]
  self.b.poll_metrics=lambda:calls.append('metrics')
  self.b.read=lambda:(calls.append('read'),f.set_result(b'ok'))
  self.b.wait_io=lambda *args:None
  self.assertEqual(self.b.host_wait(f),b'ok');self.assertEqual(calls,['metrics','read'])
 def test_paged_selection_has_actual_index_and_atomic_publish(self):
  def reply(op,payload,kw):
   kind,offset,version=struct.unpack('<BHI',payload);total=130 if kind==0 else 0;count=min(128,total-offset)
   return struct.pack('<BIHHH',kind,42,total,offset,count)+b''.join(struct.pack('<H6sB',i,bytes([2,0,0,0,i//256,i%256]),i==129) for i in range(offset,offset+count))
  calls=self.client(reply);self.b.sync_selection()
  self.assertEqual(len(calls),3);self.assertEqual(self.b.model.selection['ap']['02:00:00:00:00:81'],{'index':129,'selected':True})
  old=self.b.model.selection
  self.client(lambda *args:struct.pack('<BIHHH',0,42,1,0,0))
  with self.assertRaisesRegex(ValueError,'Incomplete'):self.b.sync_selection()
  self.assertIs(self.b.model.selection,old)
 def test_snapshot_change_rejects_mixed_pages(self):
  def reply(op,payload,kw):
   k,o,v=struct.unpack('<BHI',payload)
   return struct.pack('<BIHHH',k,42 if not o else 43,129,o,128 if not o else 1)+b''.join(struct.pack('<H6sB',i,bytes([2,0,0,0,0,i]),0) for i in range(o,min(129,o+128)))
  self.client(reply)
  with self.assertRaisesRegex(ValueError,'changed'):self.b.sync_selection()
  self.assertEqual(self.b.model.selection,{'ap':{},'sta':{}})
 def test_selection_is_mac_based_no_retries_and_unknown_security_preserved(self):
  b=self.b;mac='02:11:22:33:44:55';b.model.aps[mac]={'bssid':mac,'ssid':'lab','channel':6,'rssi':-52}
  b.sync_selection=lambda:None
  def reply(op,payload,kw):
   self.assertTrue(kw['mutation']);r=Reader(payload);kind,selected=r.unpack('BB');address=r.take(6);ch,rssi,security,n=r.unpack('BbBB');self.assertEqual(security,255)
   return struct.pack('<BH6sB',kind,71,address,selected)
  calls=self.client(reply);result=b.select_devices([{'kind':'ap','mac':mac}]);self.assertEqual(result['items'][0]['index'],71);self.assertEqual(len(calls),1)
  calls=self.client(lambda *args:(_ for _ in ()).throw(TimeoutError('unknown')))
  with self.assertRaisesRegex(RuntimeError,'未自动重试'):b.select_devices([{'kind':'ap','mac':mac}])
  self.assertEqual(len(calls),1)
 def test_invalid_selection_never_reaches_usb(self):
  calls=self.client(lambda *args:b'')
  for items in [[],[{'kind':'ble','mac':'02:11:22:33:44:55'}],[{'kind':'ap','mac':'x'}],[{'kind':'ap','mac':'02:11:22:33:44:55'}]*65]:
   with self.assertRaises(ValueError):self.b.select_devices(items)
  self.assertFalse(calls)
 def test_external_cli_event_only_marks_dirty(self):
  calls=self.client(lambda *args:b'');self.b.host_event(Frame(Channel.COMMAND,Kind.EVENT,Op.SELECTION_CHANGED,1))
  self.assertTrue(self.b.selection_dirty);self.assertFalse(calls)
 def test_native_console_flag_goes_to_binary_manager(self):
  calls=[];self.b.capture_action=lambda *args,**kw:calls.append((args,kw)) or {'active':True}
  self.b.console('sniffprobe --host-only');self.assertEqual(calls,[(('start',),{'host_only':True,'native_request':bytes([3,0,0])})]);self.assertEqual(self.b.ser.commands,[])
  with self.assertRaises(ValueError):self.b.console('capture start --host-only')
 def test_host_only_rejected_without_capability_before_radio_or_usb(self):
  self.b.update(capabilities=[])
  with self.assertRaisesRegex(ValueError,'不支持'):self.b.capture_action('start',host_only=True)
  self.assertEqual(self.b.ser.commands,[])
 def test_name_retention_and_hidden_identity(self):
  m=Model();a='02:11:22:33:44:55';m.log(f'-60 Ch: 6 {a} ESSID: lab');m.log(f'-60 Ch: 6 {a} ESSID: ')
  self.assertTrue(m.aps[a]['named']);self.assertEqual(m.aps[a]['ssid'],'lab')
  m.log('@BLE:'+json.dumps(dict(kind='device',mac=a,address_type=1,name='6c6162',rssi=-60)))
  m.log('@BLE:'+json.dumps(dict(kind='device',mac=a,address_type=1,name='',rssi=-60)))
  self.assertEqual(m.ble[a+'/1']['name'],'lab')
 def test_clear_logs_cursor_does_not_replay_or_clear_observations(self):
  m=Model();m.aps['test']={};m.log('before');cursor=m.seq
  with m.lock:m.logs.clear();m.log_cleared=m.seq;m.rev+=1
  m.log('after');snapshot=m.snapshot(0)
  self.assertEqual(snapshot['log_cleared'],cursor);self.assertEqual([x['text'] for x in snapshot['logs']],['after']);self.assertIn('test',m.aps)

import tempfile,time,unittest
from pathlib import Path
from unittest.mock import Mock,patch
from server import Bridge
class LinkSettingsTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory();self.b=Bridge(settings_path=Path(self.temp.name)/'settings.json');self.b.running=False;self.b.thread.join(1)
  self.b.update(verified=True,mode='idle',manual=False,telemetry_enabled=True)
 def tearDown(self):self.b.close();self.temp.cleanup()
 def test_distinct_intervals_and_validation(self):
  b=self.b;b.telemetry(client='testclient',view='overview',idle_refresh_interval=3,work_refresh_interval=17)
  self.assertEqual(b.metrics_interval(time.monotonic()),3);b.update(manual=True);self.assertEqual(b.metrics_interval(time.monotonic()),17)
  b.telemetry(work_refresh_interval=0);self.assertEqual(b.metrics_interval(time.monotonic()),0)
  for value in [-1,61,True,1.5,'2']:
   with self.assertRaises(ValueError):b.telemetry(idle_refresh_interval=value)
  self.assertEqual(b.settings.get('idle_refresh_interval'),3)
 def test_switch_rejected_during_manual_scan_and_pending_transfer(self):
  b=self.b
  for state in [{'manual':True},{'manual':False,'mode':'scanall'},{'mode':'idle'}]:
   b.update(**state)
   if state=={'mode':'idle'}:b.transfers.items['test']={'status':'CREATED'}
   self.assertFalse(b.idle_for_link_change())
   with self.assertRaisesRegex(ValueError,'任务执行中'):b.configure_transport('usb')
 def test_override_is_not_saved_and_auto_returns(self):
  b=self.b;b.host_client=Mock();b.link_override='uart';b.update(link_override='uart')
  b.configure_transport('auto');self.assertEqual(b.link_override,'auto');self.assertIsNone(b.settings.get('link_override'))
 def test_auto_discovery_skipped_while_busy(self):
  b=self.b;b.auto_link_enabled=True;b.update(manual=True);b.ser=Mock();b.update(port='/dev/test')
  with patch('server.ports',return_value=[{'path':'/dev/test','label':'USB'}]),patch.object(b,'probe_idle_identity') as probe:
   b.maintain_idle_links();probe.assert_not_called()

 def test_transient_identity_timeout_is_retried(self):
  b=self.b;b.auto_link_enabled=True;b.auto_link_device='same-device';b.ser=Mock();b.host_client=Mock();b.host_client.limits={'endpoint':0};b.update(port='/dev/usb')
  candidates=[{'path':'/dev/usb','label':'USB'},{'path':'/dev/uart','label':'UART'}]
  with patch('server.ports',return_value=candidates),patch.object(b,'probe_idle_identity',side_effect=[None,'same-device']) as probe,patch.object(b,'release_host_connection'),patch.object(b,'connect') as connect:
   b.maintain_idle_links();self.assertNotIn('/dev/uart',b.link_probe_ignored);connect.assert_not_called()
   b.next_link_check=0;b.maintain_idle_links();self.assertEqual(probe.call_count,2);connect.assert_called_once_with('/dev/uart',expected_device='same-device')
 def test_confirmed_other_device_is_not_promoted(self):
  b=self.b;b.auto_link_enabled=True;b.auto_link_device='same-device';b.ser=Mock();b.host_client=Mock();b.host_client.limits={'endpoint':0};b.update(port='/dev/usb')
  with patch('server.ports',return_value=[{'path':'/dev/usb','label':'USB'},{'path':'/dev/uart','label':'UART'}]),patch.object(b,'probe_idle_identity',return_value='another-device'),patch.object(b,'connect') as connect:
   b.maintain_idle_links();self.assertIn('/dev/uart',b.link_probe_ignored);connect.assert_not_called()

 def test_data_select_enxio_preserves_control(self):
  b=self.b;b.ser=Mock();b.ser.fileno.return_value=101;b.data_ser=Mock();b.data_ser.fileno.return_value=102
  with patch('server.select.select',side_effect=[OSError(6,'Device not configured'),OSError(6,'Device not configured'),([],[],[])]) as select,patch.object(b,'data_failed') as failed:
   b.wait_io();failed.assert_called_once();self.assertNotIn(102,select.call_args.args[0]);self.assertIn(101,select.call_args.args[0])
 def test_control_select_error_is_not_hidden(self):
  b=self.b;b.ser=Mock();b.ser.fileno.return_value=101;b.data_ser=Mock();b.data_ser.fileno.return_value=102
  with patch('server.select.select',side_effect=[OSError(6,'Device not configured'),([],[],[])]),patch.object(b,'data_failed') as failed:
   with self.assertRaises(OSError):b.wait_io()
   failed.assert_not_called()
 def test_reinserted_uart_quarantine_recovers_before_identity_probe(self):
  import json,struct
  from host_connection import Frame,Channel,Kind,Op,string
  notice=Frame(Channel.CONTROL,Kind.EVENT,Op.RECOVERY_NOTICE,77,0,2,string('02:00:00:00:00:01')+string('a'*32)+struct.pack('<IB',11,1)).encode()
  class Probe:
   def __init__(self):self.buffer=b'';self.writes=0;self.closed=False
   def open(self):pass
   @property
   def in_waiting(self):return len(self.buffer)
   def write(self,data):
    self.writes+=1
    if self.writes==1:self.buffer=notice
    else:self.buffer=('@MARAUDER:'+json.dumps({'tx':data.decode().strip().split()[-1],'Host_Connection':{'device_id':'same-device'}})+'\n').encode()
    return len(data)
   def read(self,n):data=self.buffer[:n];self.buffer=self.buffer[n:];return data
   def close(self):self.closed=True
  probe=Probe();b=self.b
  with patch('server.subprocess.run',return_value=Mock(stdout='')),patch('server.EndpointSerial',return_value=probe),patch.object(b,'read'),patch.object(b,'wait_io'),patch.object(b,'recover_data_endpoint') as recover:
   self.assertEqual(b.probe_idle_identity('/dev/uart'),'same-device');self.assertEqual(recover.call_args.kwargs,{'expected_endpoint':1});self.assertTrue(probe.closed);self.assertEqual(probe.writes,2)

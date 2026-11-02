import json,tempfile,time,unittest
from pathlib import Path
from unittest.mock import patch
from server import Bridge,Model
from host_storage import DeskSettings
from test_server import FakeSerial
class DeviceTests(unittest.TestCase):
 def setUp(self):
  self.b=Bridge();self.b.running=False;self.b.thread.join(1);self.b.ser=FakeSerial();self.b.update(verified=True,connected=True,mode='idle',manual=False,telemetry_enabled=True,capabilities=['device-metrics-v1']);self.b.telemetry_clients={};self.b.next_metrics=0
 def tearDown(self):self.b.close()
 def test_multiple_clients_share_one_poll(self):
  for i in range(3):self.b.telemetry(client='client000'+str(i),view='overview')
  self.b.next_metrics=0;self.b.poll_metrics();self.b.poll_metrics();self.assertEqual(len(self.b.ser.commands),1)
  self.assertEqual(self.b.metrics_interval(time.monotonic()),2)
 def test_downshift_hidden_and_expired(self):
  b=self.b;b.telemetry(client='client000',view='overview');b.telemetry(client='client000',view='background');self.assertEqual(b.metrics_interval(time.monotonic()),0);self.assertEqual(b.next_metrics,0)
  b.telemetry(client='client000',view='hidden');b.next_metrics=0;b.poll_metrics();self.assertEqual(b.ser.commands,[])
  b.telemetry(client='client000',view='overview');self.assertEqual(b.metrics_interval(time.monotonic()+26),0)
 def test_global_disable_is_zero_serial_traffic_and_persists(self):
  b=self.b;b.telemetry(client='client000',view='overview')
  with tempfile.TemporaryDirectory() as td,patch('server.ROOT',Path(td)):
   b.settings=DeskSettings(Path(td)/'.desk-settings.json')
   b.telemetry(enabled=False);b.next_metrics=0
   for _ in range(10):b.poll_metrics()
   self.assertEqual(b.ser.commands,[]);self.assertFalse(json.loads((Path(td)/'.desk-settings.json').read_text())['telemetry_enabled'])
 def test_manual_and_old_firmware_have_no_injected_queries(self):
  b=self.b;b.telemetry(client='client000',view='overview');b.next_metrics=0;b.update(manual=True);b.poll_metrics();self.assertEqual(b.ser.commands,[])
  b.update(manual=False,capabilities=[]);b.poll_metrics();self.assertEqual(b.ser.commands,[])
 def test_metrics_are_bounded_and_not_console_spam(self):
  m=Model();sample=dict(v=1,tx='desk_metrics1',uptime_ms=12,heap_total=200,heap_free=100,psram_total=800,psram_free=700,storage_total=20,storage_used=3,temperature_c=44.2)
  for _ in range(150):m.log('@DEVICE:'+json.dumps(sample))
  self.assertEqual(len(m.metric_history),120);self.assertEqual(len(m.logs),0);self.assertEqual(m.metrics['temperature_c'],44.2)
  self.assertEqual(m.state['last_rx'],m.metrics['received_at'])
  m.log('@DEVICE:bad');self.assertEqual(m.metrics['temperature_c'],44.2)
 def test_ble_payload_decoding_and_status(self):
  m=Model();d=dict(kind='device',mac='aa:bb:cc:dd:ee:ff',address_type=1,name='e4b8ade69687',rssi=-70,age_ms=0)
  m.log('@BLE:'+json.dumps(d));self.assertEqual(m.ble['aa:bb:cc:dd:ee:ff/1']['name'],'中文')
  d['name']='nothex';m.log('@BLE:'+json.dumps(d));self.assertEqual(len(m.ble),1)
  m.log('@BLE:{"kind":"status","active":true}');self.assertEqual(m.state['mode'],'bleobserve')
  m.log('@BLE:{"kind":"clear"}');self.assertEqual(len(m.ble),0)
 def test_radio_start_is_mutually_exclusive(self):
  b=self.b;b.update(mode='bleobserve')
  with self.assertRaisesRegex(ValueError,'停止蓝牙'):b.command('scancombined')
  b.update(mode='scanall',capabilities=['ble-observer-v1'])
  with self.assertRaisesRegex(ValueError,'停止当前'):b.command('bleobserve start')
  self.assertEqual(b.ser.commands,[])
 def test_invalid_subscription_does_not_touch_usb(self):
  for bad in ['x','../file','a'*100]:
   with self.assertRaises(ValueError):self.b.telemetry(client=bad,view='overview')
  self.assertEqual(self.b.ser.commands,[])

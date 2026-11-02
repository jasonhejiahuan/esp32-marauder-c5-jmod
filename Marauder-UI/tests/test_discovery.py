import unittest,json,time
from unittest.mock import patch
from server import Model,Bridge
class DiscoveryTests(unittest.TestCase):
 def event(self,**kw):
  return dict(k='a',mac='02:11:22:33:44:55',ssid='54657374',sec=3,cap=17,r=-88,ch=149,n=10,age=50,**kw)
 def test_counter_and_age(self):
  m=Model();m.begin_round('scancombined',at=100);d=self.event();m.discovery(json.dumps(d),102)
  a=m.aps[d['mac']];self.assertEqual(a['samples'],10);self.assertEqual(a['last'],101.95);self.assertEqual(a['security'],'WPA2')
  d['n']=16;m.discovery(json.dumps(d),104);self.assertEqual(m.aps[d['mac']]['samples'],16)
  m.discovery(json.dumps(d),105);self.assertEqual(m.aps[d['mac']]['samples'],16)
 def test_invalid_never_adds(self):
  for change in [{'ssid':'ff'*33},{'age':-1},{'mac':'ff:ff:ff:ff:ff:ff'},{'ch':999},{'ssid':'bad'},{'r':False}]:
   m=Model();d=self.event();d.update(change);m.discovery(json.dumps(d),100);self.assertFalse(m.aps)
 def test_delayed_observation_not_current_round(self):
  m=Model();m.begin_round('scancombined',at=100);d=self.event();d['age']=2000;m.discovery(json.dumps(d),101);self.assertFalse(m.round['aps'])
 def test_status_required_and_overflow_prevents_absence(self):
  m=Model();m.begin_round('scancombined',at=100);m.discovery(json.dumps(self.event()),101);self.assertFalse(m.finish_round(at=131))
  m.begin_round('scancombined',at=131);m.discovery(json.dumps(dict(k='status',beacons=10,data=2,overflow=1)),160);self.assertFalse(m.finish_round(at=161))
 def test_two_complete_rounds_and_station_link(self):
  m=Model();m.begin_round('scancombined',at=100);m.discovery(json.dumps(self.event()),101)
  m.discovery(json.dumps(dict(k='s',mac='04:11:22:33:44:55',ap='02:11:22:33:44:55',r=-60,ch=149,n=7,age=0)),102)
  self.assertEqual(m.aps['02:11:22:33:44:55']['station_count'],1)
  for start in [100,130,160]:
   if start!=100:m.begin_round('scancombined',at=start)
   m.discovery(json.dumps(dict(k='status',beacons=10,data=7,overflow=0)),start+29)
   self.assertTrue(m.finish_round(at=start+30))
  self.assertFalse(m.aps['02:11:22:33:44:55']['online']);self.assertFalse(m.stations['04:11:22:33:44:55']['online'])
 def test_new_mode_round_does_not_stop_radio(self):
  from test_server import FakeSerial
  class CombinedSerial(FakeSerial):
   def write(self,data):
    if data==b'scancombined\n':
     self.commands.append('scancombined');self.buffer+=b'Starting AP + STA + Beacon; discovery-v1; LED=00FFFF\n';return len(data)
    return super().write(data)
  b=Bridge();b.ser=CombinedSerial();b.update(verified=True,connected=True,mode='idle',capabilities=['discovery-v1'])
  try:
   b.call('command',command='scancombined',duration=60)
   self.assertEqual(b.ser.commands,['stopscan','scancombined'])
   b.model.round['started']=time.time()-31;b.model.state['combined_status_at']=time.time();b.round_until=time.time()-.1
   end=time.monotonic()+1
   while b.model.state['completed_rounds']==0 and time.monotonic()<end:time.sleep(.01)
   self.assertEqual(b.model.state['completed_rounds'],1);self.assertEqual(b.ser.commands,['stopscan','scancombined'])
  finally:b.running=False;b.thread.join(1);b.close()
 def test_old_firmware_rejects_only_new_mode(self):
  from test_server import FakeSerial
  b=Bridge();b.ser=FakeSerial();b.update(verified=True,mode='idle')
  try:
   with self.assertRaisesRegex(RuntimeError,'不支持'):b.call('command',command='scancombined')
   self.assertEqual(b.ser.commands,[])
  finally:b.running=False;b.thread.join(1);b.close()

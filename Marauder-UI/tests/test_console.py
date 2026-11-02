import unittest,time
from server import Model,Bridge
from test_server import FakeSerial
class ConsoleTests(unittest.TestCase):
 def setUp(self):
  self.b=Bridge();self.b.ser=FakeSerial();self.b.update(verified=True,connected=True,mode='idle')
 def tearDown(self):self.b.running=False;self.b.thread.join(1);self.b.close()
 def test_arbitrary_single_line_preserves_arguments(self):
  for cmd in ['settings','info -a 3','led -s #80FFFF','future_command "two words"']:
   self.b.call('console',command=cmd)
  self.assertEqual(self.b.ser.commands,['settings','info -a 3','led -s #80FFFF','future_command "two words"'])
 def test_manual_cancels_host_round_without_extra_serial_commands(self):
  b=self.b;b.capture_mode='scanall';b.round_until=time.time()+.05;b.model.begin_round('scanall');b.update(deadline=time.time()+.05,mode='scanall')
  b.call('console',command='help');time.sleep(.15)
  self.assertEqual(b.ser.commands,['help']);self.assertIsNone(b.model.round);self.assertIsNone(b.model.state['deadline']);self.assertTrue(b.model.state['manual']);self.assertEqual(b.model.state['completed_rounds'],0)
 def test_manual_stop_returns_idle(self):
  self.b.call('console',command='help');self.b.call('console',command='stopscan')
  self.assertFalse(self.b.model.state['manual']);self.assertEqual(self.b.model.state['mode'],'idle')
 def test_invalid_lines_do_not_write(self):
  for cmd in [None,'','info\nreboot','help\r','help\x00','a'*2049]:
   with self.assertRaises(RuntimeError):self.b.call('console',command=cmd)
  self.assertEqual(self.b.ser.commands,[])
 def test_requires_verified_device(self):
  self.b.update(verified=False)
  with self.assertRaises(RuntimeError):self.b.call('console',command='help')
  self.assertEqual(self.b.ser.commands,[])
 def test_help_catalog_commits_only_complete_frame(self):
  m=Model();m.log('============ Commands ============');m.log('channel [-s <channel>]');m.log('scancombined (AP + STA + Beacon; discovery-v1)');m.log('  ignore continuation');m.log('led -s <hex>')
  self.assertEqual(m.state['command_names'],[])
  m.log('==================================')
  self.assertEqual(m.state['command_names'],['channel','help','led','scancombined','stopscan'])
  self.assertIn('channel [-s <channel>]',m.state['command_help'])
  self.assertIn('led -s <hex>',m.state['command_help'])

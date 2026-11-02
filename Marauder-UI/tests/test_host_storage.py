import json,os,stat,tempfile,threading,unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from host_storage import DeskSettings,prepare_directory,remove_empty
from server import Bridge
from capture import CaptureSink
from transfers import VerifiedSink

class HostStorageTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name).resolve();self.config=self.root/'settings.json'
  self.config.write_text(json.dumps({'telemetry_enabled':True,'unrelated':'preserve'}))
  self.b=Bridge(temp_directory=str(self.root/'initial'),settings_path=self.config);self.b.running=False;self.b.thread.join(1)
 def tearDown(self):self.b.close();self.tmp.cleanup()
 def test_custom_directory_is_private_without_changing_parent_permissions(self):
  parent=self.root/'parent';parent.mkdir(mode=0o755);before=stat.S_IMODE(parent.stat().st_mode)
  result=self.b.host_storage('set',str(parent));directory=Path(result['directory'])
  self.assertEqual(directory.parent,parent);self.assertEqual(stat.S_IMODE(directory.stat().st_mode),0o700)
  self.assertEqual(stat.S_IMODE(parent.stat().st_mode),before)
  self.assertEqual(self.b.transfers.directory,directory);self.assertEqual(self.b.model.state['host_storage'],result)
 def test_existing_sinks_remain_readable_and_future_sinks_use_new_directory(self):
  old=Path(self.b.host_storage_state['directory']);file=old/'kept';file.write_bytes(b'original')
  result=self.b.host_storage('set',str(self.root/'chosen'))
  self.assertEqual(file.read_bytes(),b'original')
  sink=CaptureSink(result['directory'],1,1);sink.close();self.assertEqual(sink.path.parent,Path(result['directory']));self.assertEqual(sink.path.stat().st_size,24)
  path=self.b.transfers.directory/'download';download=VerifiedSink(path,0,'test');download.close();self.assertTrue(path.exists());self.assertTrue(old.exists())
 def test_settings_merge_and_restart_use_saved_directory(self):
  chosen=str(self.root/'chosen');self.b.host_storage('set',chosen);self.b.telemetry(enabled=False)
  values=json.loads(self.config.read_text());self.assertEqual(values,{'unrelated':'preserve','telemetry_enabled':False,'temp_directory':chosen})
  b=Bridge(settings_path=self.config);b.running=False;b.thread.join(1)
  try:self.assertEqual(b.host_storage_state['base_directory'],chosen);self.assertFalse(b.model.state['telemetry_enabled'])
  finally:b.close()
 def test_override_does_not_persist_until_saved(self):
  state=self.b.host_storage();self.assertNotIn('temp_directory',json.loads(self.config.read_text()))
  self.b.host_storage('set',state['configured']);self.assertEqual(json.loads(self.config.read_text())['temp_directory'],state['configured'])
 def test_invalid_paths_do_not_change_live_directory(self):
  before=self.b.host_storage();file=self.root/'file';file.write_text('keep')
  for value in ['relative',None,123,'bad\0path',str(file),str(file/'child')]:
   with self.subTest(value=value),self.assertRaises(ValueError):self.b.host_storage('set',value)
  self.assertEqual(self.b.host_storage(),before);self.assertEqual(file.read_text(),'keep')
 def test_capture_or_file_worker_blocks_switch(self):
  path=str(self.root/'not-created');before=self.b.host_storage()
  self.b.capture_history.append(SimpleNamespace(running=True,thread=SimpleNamespace(is_alive=lambda:True)))
  with self.assertRaisesRegex(ValueError,'先结束'):self.b.host_storage('set',path)
  self.b.capture_history.clear();self.b.transfers.items['test']={'status':'COMPLETED','worker':SimpleNamespace(is_alive=lambda:True)}
  with self.assertRaisesRegex(ValueError,'先结束'):self.b.host_storage('set',path)
  self.b.transfers.items.clear();self.assertFalse(Path(path).exists());self.assertEqual(self.b.host_storage(),before)
 def test_race_rechecks_after_filesystem_validation(self):
  import server
  original=server.prepare_directory
  def prepare(value):
   result=original(value);self.b.pending_capture=object();return result
  with patch('server.prepare_directory',side_effect=prepare),self.assertRaises(ValueError):self.b.host_storage('set',str(self.root/'raced'))
  self.b.pending_capture=None;self.assertEqual(list((self.root/'raced').iterdir()),[]);self.assertEqual(self.b.host_storage_state['configured'],str(self.root/'initial'))
 def test_save_failure_rolls_back_live_directory_and_removes_only_empty_child(self):
  before=self.b.host_storage();path=self.root/'chosen';path.mkdir();(path/'unrelated').write_text('keep')
  with patch.object(self.b.settings,'update',side_effect=OSError('disk full')),self.assertRaises(OSError):self.b.host_storage('set',str(path))
  self.assertEqual(self.b.host_storage(),before);self.assertEqual(list(path.iterdir()),[path/'unrelated'])
 def test_settings_concurrent_updates_are_merged(self):
  settings=DeskSettings(self.config);threads=[threading.Thread(target=settings.update,kwargs={f'key_{i}':i}) for i in range(12)]
  for t in threads:t.start()
  for t in threads:t.join()
  saved=json.loads(self.config.read_text());self.assertEqual(len(saved),14);self.assertEqual(stat.S_IMODE(self.config.stat().st_mode),0o600)
 def test_reset_uses_system_parent_and_no_usb_requests(self):
  with patch('host_storage.tempfile.gettempdir',return_value=str(self.root/'system')):
   result=self.b.host_storage('set','');self.assertEqual(result['configured'],'');self.assertEqual(result['base_directory'],str(self.root/'system'))
  self.assertEqual(self.b.jobs.qsize(),0)
 def test_home_expansion_and_missing_absolute_parent(self):
  with tempfile.TemporaryDirectory(prefix='.marauder-test-',dir=Path.home()) as td:
   path=Path(td).resolve()/'nested/cache';value='~/'+str(path.relative_to(Path.home().resolve()))
   result=prepare_directory(value);self.assertEqual(result['base_directory'],str(path));remove_empty(result['directory'])

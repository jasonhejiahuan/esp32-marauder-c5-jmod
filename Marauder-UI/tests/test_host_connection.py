import concurrent.futures
import hashlib
import io
import os
import random
import struct
import tempfile
import threading
import time
import unittest
import zlib
from unittest.mock import patch
from pathlib import Path
FIRMWARE_ROOT = Path(__file__).resolve().parents[2]
if not (FIRMWARE_ROOT / 'esp32_marauder').is_dir():
 FIRMWARE_ROOT = FIRMWARE_ROOT / 'Marauder-Firmware'
from host_connection import *
from transport import HostConnectionClient,IndeterminateError
from transfers import VerifiedSink,TransferManager
from capture import CaptureSink

class CodecTests(unittest.TestCase):
 def test_crc_and_cobs_vectors(self):
  self.assertEqual(zlib.crc32(b'123456789'),0xcbf43926)
  for raw,encoded in [(b'',b'\1'),(b'\0',b'\1\1'),(b'\x11\x22\0\x33',b'\3\x11\x22\2\x33'),(b'\0\0',b'\1\1\1')]:
   self.assertEqual(cobs_encode(raw),encoded);self.assertEqual(cobs_decode(encoded),raw)
 def test_explicit_header_and_crc(self):
  f=Frame(1,1,0x210,0x12345678,7,9,b'\0abc');raw=cobs_decode(f.encode()[:-1])
  self.assertEqual(raw[:20],bytes.fromhex('0101010010020400785634120700000009000000'))
  self.assertEqual(Frame.decode(f.encode()[:-1]),f)
 def test_fragment_coalesce_all_splits(self):
  frames=[Frame(1,1,0x100,17,i,i,bytes(range(256))*i) for i in range(1,5)]
  data=b''.join(f.encode() for f in frames)
  for split in range(len(data)+1):
   d=Decoder();self.assertEqual(list(d.feed(data[:split]))+list(d.feed(data[split:])),frames)
 def test_bound_missing_delimiter_and_resync(self):
  d=Decoder();self.assertEqual(list(d.feed(b'A'*(MAX_ENCODED*3))),[]);self.assertLessEqual(len(d.buffer),MAX_ENCODED)
  f=Frame(0,1,2,19,3,1);self.assertEqual(list(d.feed(b'\0'+f.encode())),[f]);self.assertEqual(d.oversized,1)
 def test_corrupt_length_version_crc_no_cli(self):
  f=Frame(0,1,2,19,3,1,b'reboot\n');raw=bytearray(cobs_decode(f.encode()[:-1]))
  for index in [0,1,2,3,6,8,20,len(raw)-1]:
   bad=raw.copy();bad[index]^=0x80;d=Decoder();self.assertEqual(list(d.feed(cobs_encode(bad)+b'\0'+f.encode())),[f]);self.assertEqual(d.errors,1)
 def test_fuzz_bounded_recovery(self):
  rng=random.Random(71);d=Decoder();f=Frame(0,1,2,19,3,1)
  for _ in range(500):
   data=rng.randbytes(rng.randrange(12000));list(d.feed(data+b'\0'))
   self.assertEqual(list(d.feed(f.encode())),[f]);self.assertLessEqual(len(d.buffer),MAX_ENCODED)
 def test_payload_read_bounds(self):
  for payload in [b'',b'\xff\xffa',b'\x02\0\0x']:
   with self.assertRaises(ValueError):Reader(payload).string(160)
  r=Reader(b'\1');self.assertRaises(ValueError,r.end)

class PartialSerial:
 def __init__(self,limit=3):self.bytes=bytearray();self.limit=limit
 def write(self,data):n=min(self.limit,len(data));self.bytes.extend(data[:n]);return n

class AsyncClientTests(unittest.TestCase):
 def client(self):return HostConnectionClient(77,lambda:None)
 def reply(self,c,request,op,payload=b'',sequence=1,session=77,kind=Kind.RESULT):
  channel=c.pending[request].channel if request in c.pending else 1
  c.feed(Frame(channel,kind,op,session,request,sequence,payload).encode())
 def test_partial_writes_not_interleaved(self):
  c=self.client();f=c.request(Op.CLI_EXEC,string('help'));s=PartialSerial();c.pump(s);g=c.request(Op.PING,channel=Channel.CONTROL)
  for _ in range(200):c.pump(s)
  frames=list(Decoder().feed(s.bytes));self.assertEqual([f.request for f in frames],[1,2]);self.assertGreater(c.metrics['partial_writes'],0)
  self.reply(c,2,Op.PING,sequence=1);self.reply(c,1,Op.CLI_EXEC,sequence=2);self.assertTrue(g.done());self.assertTrue(f.done())
 def test_concurrent_responses_and_capture_keep_draining(self):
  c=self.client();events=[];c.on_event=events.append
  futures=[c.request(Op.STATUS,channel=Channel.CONTROL) for _ in range(10)]
  frames=[Frame(2,Kind.DATA,Op.CAPTURE_RECORD,77,0,1,b'packet')]
  frames += [Frame(0,Kind.RESULT,Op.STATUS,77,i,i+2,b'ok') for i in reversed(range(1,11))]
  # Sequence is per link, independent of request completion order.
  frames=[dataclasses.replace(f,sequence=i+1) for i,f in enumerate(frames)]
  c.feed(b''.join(f.encode() for f in frames));self.assertEqual(len(events),1);self.assertTrue(all(f.result()==b'ok' for f in futures))
 def test_stale_session_and_sequence(self):
  c=self.client();f=c.request(Op.PING);self.reply(c,1,Op.PING,session=76);self.assertFalse(f.done())
  self.reply(c,1,Op.PING);self.assertTrue(f.done());g=c.request(Op.PING);self.reply(c,2,Op.PING,sequence=1);self.assertFalse(g.done())
 def test_mutation_timeout_indeterminate_no_retry(self):
  c=self.client();future=c.request(Op.FILE_OP,b'x',timeout=0,mutation=True);c.pump(PartialSerial())
  self.assertRaises(IndeterminateError,future.result);self.assertEqual(c.next_request,2)
 def test_bounded_pending_and_disconnect(self):
  c=self.client();futures=[c.request(Op.PING,channel=Channel.CONTROL) for _ in range(32)];self.assertRaises(BufferError,c.request,Op.PING)
  c.close();self.assertTrue(all(f.done() for f in futures));self.assertEqual(c.tx_bytes,0)
 def test_response_mismatch_and_limits(self):
  c=self.client();f=c.request(Op.PING);self.reply(c,1,Op.CLOSE);self.assertRaises(ValueError,f.result)
  self.assertRaises(ValueError,c.accept_hello,string('02:00:00:00:00:01')+struct.pack('<HIIBBI',1024,100000,50,1,0,1))

class SinkTests(unittest.TestCase):
 def test_file_verified_stream_and_all_invalid_ranges(self):
  with tempfile.TemporaryDirectory() as d:
   s=VerifiedSink(Path(d)/'x',6,('device',1,'/x'));s.record(s.identity,0,b'abc')
   for identity,offset,data in [(s.identity,0,b'abc'),(s.identity,2,b'a'),(s.identity,4,b'a'),(s.identity,3,b'xxxx'),(('other',),3,b'def')]:
    with self.assertRaises(ValueError):s.record(identity,offset,data)
   s.record(s.identity,3,b'def');self.assertRaises(ValueError,s.complete,6,b'0'*32)
   self.assertEqual(s.complete(6,hashlib.sha256(b'abcdef').digest()),hashlib.sha256(b'abcdef').hexdigest());s.close()
 def test_empty_file(self):
  with tempfile.TemporaryDirectory() as d:
   s=VerifiedSink(Path(d)/'x',0,1);s.complete(0,hashlib.sha256(b'').digest());s.close()
 def test_capture_valid_boundaries_relative_clock(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,1,2)
   for seq in [0,3]:sink.accept(struct.pack('<IIIQHHbB',1,seq,2,2000001,3,3,-50,36)+b'abc')
   sink.close();data=sink.path.read_bytes();self.assertEqual(len(data),24+2*19);self.assertEqual(sink.stats['sequence_gaps'],2)
   self.assertEqual(struct.unpack_from('<IIII',data,24),(2,1,3,3));self.assertEqual(struct.unpack_from('<I',data,20)[0],105)
 def test_slow_sink_isolated_full_queue(self):
  gate=threading.Event()
  class Slow:
   def write(self,data):gate.wait(3);return len(data)
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,1,2,writer=Slow());started=time.monotonic()
   for seq in range(1000):sink.accept(struct.pack('<IIIQHHbB',1,seq,2,5,1,1,-50,1)+b'a')
   self.assertLess(time.monotonic()-started,1);self.assertLessEqual(sink.queue.qsize(),128);self.assertGreater(sink.stats['backend_drops'],0)
   gate.set();sink.close()
 def test_capture_bad_identity_lengths_and_sequence(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,1,2)
   for payload in [b'',struct.pack('<IIIQHHbB',2,0,2,5,1,1,-50,1)+b'a',struct.pack('<IIIQHHbB',1,0,2,5,1,2,-50,1)+b'aa']:
    sink.accept(payload)
   self.assertEqual(sink.stats['invalid_records'],3);sink.close()

class TransferLifecycleTests(unittest.TestCase):
 class Bridge:
  def __init__(self,data=b'abcdef',changed=False,gate=None):
   self.data=data;self.changed=changed;self.reads=0;self.gate=gate;self.host_client=None
   self.model=type('Model',(),{'instance':'test','state':{'port':'synthetic'}})()
  @staticmethod
  def file_path(path):
   if not isinstance(path,str) or not path.startswith('/'):raise ValueError('path')
  def call(self,action,**kw):
   if self.gate:self.gate.wait(2)
   if kw['operation']=='stat':return {'size':len(self.data)}
   self.reads+=1;data=self.data[kw['offset']:kw['offset']+kw['length']]
   if self.changed and self.reads>1:data=b'x'*len(data)
   import base64
   return dict(size=len(self.data),offset=kw['offset'],data=base64.b64encode(data).decode())
 def wait(self,m,key):
  end=time.monotonic()+3
  while time.monotonic()<end:
   t=m.request('status',transfer_id=key)
   if t['status'] in {'COMPLETED','FAILED','CANCELLED'}:return t
   time.sleep(.005)
  self.fail('transfer did not terminate')
 def test_backend_owns_download_after_browser_leaves(self):
  with tempfile.TemporaryDirectory() as d:
   m=TransferManager(self.Bridge(),d);t=m.request('start','/x');t=self.wait(m,t['transfer_id'])
   self.assertEqual(t['status'],'COMPLETED');self.assertEqual(t['offset'],6)
   file,size,name=m.download(t['transfer_id']);self.assertEqual(file.read(),b'abcdef');file.close();m.close()
 def test_source_change_fails_and_discards_only_own_temp(self):
  with tempfile.TemporaryDirectory() as d:
   protected=Path(d)/'keep';protected.write_bytes(b'keep');m=TransferManager(self.Bridge(changed=True),d)
   t=m.request('start','/x');t=self.wait(m,t['transfer_id']);self.assertEqual(t['status'],'FAILED');self.assertTrue(protected.exists());self.assertFalse((Path(d)/t['transfer_id']).exists())
 def test_cancel_has_clear_terminal_and_bounded_concurrency(self):
  gate=threading.Event()
  with tempfile.TemporaryDirectory() as d:
   m=TransferManager(self.Bridge(gate=gate),d);t=m.request('start','/x')
   self.assertRaises(ValueError,m.request,'start','/y');self.assertEqual(m.request('cancel',transfer_id=t['transfer_id'])['status'],'CANCELLING')
   gate.set();t=self.wait(m,t['transfer_id']);self.assertEqual(t['status'],'CANCELLED');self.assertFalse((Path(d)/t['transfer_id']).exists());m.close()

class PriorityAndRecoveryTests(unittest.TestCase):
 def test_reserved_cancel_overtakes_unstarted_requests_at_frame_boundary(self):
  c=HostConnectionClient(77,lambda:None);futures=[c.request(Op.STATUS) for _ in range(24)]
  self.assertRaises(BufferError,c.request,Op.STATUS)
  cancel=c.request(Op.CAPTURE_STOP,channel=Channel.CONTROL);s=PartialSerial(4096)
  while c.tx_bytes:c.pump(s)
  frames=list(Decoder().feed(s.bytes));self.assertEqual(frames[0].op,Op.CAPTURE_STOP)
  self.assertEqual([f.sequence for f in frames],list(range(1,26)));c.close()
 def test_expired_unsent_mutation_is_not_sent_late(self):
  c=HostConnectionClient(77,lambda:None);f=c.request(Op.FILE_OP,b'x',timeout=0,mutation=True);s=PartialSerial();c.pump(s)
  self.assertEqual(s.bytes,b'');self.assertRaises(IndeterminateError,f.result)
 def test_session_refresh_failure_fails_remaining_requests(self):
  c=HostConnectionClient(77,lambda:None);c.last_tx-=3;c.pump(PartialSerial())
  self.assertTrue(c.pending)
  for p in c.pending.values():p.deadline=0
  c.pump(PartialSerial());self.assertTrue(c.closed);self.assertEqual(c.metrics['session_refresh_failures'],1)
 def test_future_cancellation_does_not_break_reader(self):
  c=HostConnectionClient(77,lambda:None);f=c.request(Op.PING);f.cancel()
  c.feed(Frame(1,Kind.RESULT,Op.PING,77,1,1).encode());self.assertTrue(f.cancelled())

class BridgeCompatibilityTests(unittest.TestCase):
 def test_legacy_discovery_never_changes_radio_state_or_sends_binary(self):
  import server
  class Peer:
   def __init__(self,*a,**kw):self.buffer=b'';self.commands=[];self.port=None
   def open(self):pass
   def close(self):pass
   @property
   def in_waiting(self):return len(self.buffer)
   def read(self,n):data=self.buffer[:n];self.buffer=self.buffer[n:];return data
   def write(self,data):
    command=data.decode('utf-8','strict').strip();self.commands.append(command)
    if command.startswith('protocolinfo'):reply='@MARAUDER:'+__import__('json').dumps(dict(tx='desk',status='success',firmware='v1.18.0',capabilities=['usb-ota-v1','device-metrics-v1']))
    elif command.startswith('ota status'):reply='@OTA:'+__import__('json').dumps(dict(tx='desk_identity',ok=True,profile='waveshare-c5-rgb-usb-4m6m-v1'))
    elif command=='help':reply='============ Commands ============\nhelp\nstopscan\n===================='
    elif command.startswith('deviceinfo'):reply='@DEVICE:'+__import__('json').dumps(dict(v=1,tx='desk_identity',uptime_ms=1234,wifi_mode=89,ble_active=False,heap_total=1000,heap_free=500,psram_total=8000,psram_free=4000,storage_total=6000,storage_used=1000))
    else:raise AssertionError('Unexpected operational command '+command)
    self.buffer+=(reply+'\n').encode();return len(data)
  peer=Peer();b=server.Bridge();b.running=False;b.host_wake();b.thread.join(1)
  try:
   with patch('server.EndpointSerial',return_value=peer),patch('server.ports',return_value=[dict(path='/synthetic',label='USB',serial='test')]),patch('server.subprocess.run',return_value=type('Result',(),dict(stdout=''))()):
    self.assertTrue(b.connect('/synthetic')['ok'])
   self.assertIsNone(b.host_client);self.assertEqual(b.model.state['mode'],'scancombined')
   self.assertNotIn('stopscan',peer.commands);self.assertNotIn('info',peer.commands)
   self.assertFalse(any(c.startswith('host_connection') for c in peer.commands))
  finally:b.close()
 def test_discovery_quarantines_binary_until_explicit_recovery_notice(self):
  import server
  from test_server import FakeSerial
  b=server.Bridge();b.running=False;b.host_wake();b.thread.join(1);b.ser=FakeSerial();b.discovering=True
  try:
   raw=b'secret-capture-data\n'+b'\xff'*50
   b.ser.buffer=Frame(Channel.FILE,Kind.DATA,Op.FILE_RECORD,77,0,1,raw).encode();b.read()
   self.assertFalse(b.model.logs)
   notice=Frame(Channel.CONTROL,Kind.EVENT,Op.RECOVERY_NOTICE,77,0,2,string('02:00:00:00:00:01')+string('a'*32)+struct.pack('<IB',11,0)).encode()
   for byte in notice:b.ser.buffer=bytes([byte]);b.read()
   self.assertEqual(b.recovery_notice,(77,11,'a'*32));self.assertFalse(b.model.logs)
  finally:b.close()
 def test_binary_file_records_never_reach_text_model(self):
  import server,queue
  from test_server import FakeSerial
  b=server.Bridge();b.running=False;b.host_wake();b.thread.join(1);b.ser=FakeSerial()
  b.host_client=HostConnectionClient(77,lambda:None,b.host_console,b.host_event);b.file_records=queue.Queue(maxsize=1)
  try:
   data=b'\xff\xfeopaque-pcap-bytes\n';b.ser.buffer=Frame(Channel.FILE,Kind.DATA,Op.FILE_RECORD,77,0,1,data).encode();b.read()
   self.assertEqual(b.file_records.get_nowait().payload,data);self.assertFalse(b.model.logs)
   b.host_event(Frame(Channel.FILE,Kind.DATA,Op.FILE_RECORD,77,0,2,b'x'));b.host_event(Frame(Channel.FILE,Kind.DATA,Op.FILE_RECORD,77,0,3,b'y'))
   self.assertIn('full',b.file_error)
  finally:b.close()

class CaptureSubscriptionsTests(unittest.TestCase):
 def test_stalled_live_client_does_not_block_disk_or_another_subscriber(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,9,4);slow=sink.subscribe();fast=sink.subscribe()
   for seq in range(300):
    sink.accept(struct.pack('<IIIQHHbB',9,seq,4,seq,1,1,-70,6)+b'x')
    self.assertEqual(len(fast.get_nowait()),17)
   self.assertEqual(slow.qsize(),128);self.assertEqual(sink.stats['subscriber_drops'],172)
   sink.subscribe();sink.subscribe();self.assertRaises(ValueError,sink.subscribe)
   sink.unsubscribe(slow);sink.close();self.assertEqual(len(sink.header()),24)

class AdditionalBoundaryTests(unittest.TestCase):
 def test_unsupported_version_and_declared_oversize_with_valid_crc(self):
  good=Frame(0,1,2,77,1,1).encode()
  for change in ('version','size','flags'):
   raw=bytearray(cobs_decode(good[:-1]))
   if change=='version':raw[0]=2
   elif change=='size':struct.pack_into('<H',raw,6,65535)
   else:raw[3]=1
   struct.pack_into('<I',raw,len(raw)-4,zlib.crc32(raw[:-4]))
   d=Decoder();self.assertEqual(list(d.feed(cobs_encode(raw)+b'\0'+good)),[Frame(0,1,2,77,1,1)])
   self.assertEqual(d.errors,1)
 def test_hello_old_and_extended_capabilities(self):
  base=string('02:00:00:00:00:01')+struct.pack('<HIIBBI',3072,4096,12000,1,0,1)
  c=HostConnectionClient(1,lambda:None);self.assertEqual(c.accept_hello(base)['window'],4096)
  self.assertEqual(c.accept_hello(base+struct.pack('<IBHH',5,2,Op.FILE_OP,Op.TELEMETRY))['features'],5)
  self.assertRaises(ValueError,c.accept_hello,base+struct.pack('<IB',0,255))
 def test_data_descriptor_failure_preserves_control_and_reader_thread(self):
  import server,socket
  b=server.Bridge();b.running=False;b.host_wake();b.thread.join(1)
  control,peer=socket.socketpair()
  class Dead:
   def fileno(self):raise OSError('closed data')
   def close(self):raise OSError('already closed')
  b.ser=control;b.data_ser=Dead();b.host_client=HostConnectionClient(7,lambda:None);b.data_client=HostConnectionClient(7,lambda:None)
  try:
   b.wait_io(.001,False);self.assertEqual(b.model.state['transport'],'DATA_LINK_FAILED')
   self.assertIs(b.ser,control);self.assertFalse(b.host_client.closed);self.assertIsNone(b.data_ser)
  finally:b.close();peer.close()
 def test_request_counter_exhaustion_requires_new_session(self):
  c=HostConnectionClient(1,lambda:None);c.next_request=0xffffffff
  self.assertRaises(ConnectionError,c.request,Op.PING)
 def test_whole_file_truncation_is_not_completion(self):
  with tempfile.TemporaryDirectory() as d:
   sink=VerifiedSink(Path(d)/'x',6,1);sink.record(1,0,b'abc')
   self.assertRaises(ValueError,sink.complete,3,hashlib.sha256(b'abc').digest());sink.close()

class CompletionBoundaryTests(unittest.TestCase):
 def test_capture_end_accounts_for_final_dropped_records_and_drains_sink(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,3,4)
   for seq in [0,2]:sink.accept(struct.pack('<IIIQHHbB',3,seq,4,seq,1,1,-50,1)+b'x')
   sink.finish(struct.pack('<IIIII',3,4,5,3,2));sink.close()
   self.assertEqual(sink.stats['sequence_gaps'],3)
   self.assertEqual(sink.stats['transport_missing_records'],0)
   self.assertEqual(sink.stats['written'],2)
   self.assertEqual(sink.path.stat().st_size,24+2*17)
 def test_capture_end_counts_missing_transport_records_separately(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,3,4)
   sink.accept(struct.pack('<IIIQHHbB',3,0,4,1,1,1,-50,1)+b'x')
   sink.finish(struct.pack('<IIIII',3,4,4,1,3));sink.close()
   self.assertEqual(sink.stats['sequence_gaps'],3)
   self.assertEqual(sink.stats['transport_missing_records'],2)
 def test_download_start_id_persisted_before_response_reuses_same_backend_work(self):
  gate=threading.Event()
  with tempfile.TemporaryDirectory() as d:
   manager=TransferManager(TransferLifecycleTests.Bridge(gate=gate),d);key='a'*32
   first=manager.request('start','/x',key);again=manager.request('start','/x',key)
   self.assertEqual(first['transfer_id'],again['transfer_id']);self.assertEqual(len(manager.items),1)
   self.assertRaises(ValueError,manager.request,'start','/y',key)
   self.assertRaises(ValueError,manager.request,'start','/x','../bad')
   gate.set();manager.items[key]['worker'].join(3)
   self.assertEqual(manager.request('status',transfer_id=key)['status'],'COMPLETED')
   self.assertEqual(manager.request('start','/x',key)['status'],'COMPLETED');manager.close()

class UpdaterRecoveryTests(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  import importlib.util
  source=FIRMWARE_ROOT/'tools/host_connection_recovery.py'
  spec=importlib.util.spec_from_file_location('ota_recovery',source);cls.recovery=importlib.util.module_from_spec(spec);spec.loader.exec_module(cls.recovery)
 def test_standalone_codec_matches_protocol_and_rejects_untrusted_notice(self):
  recovery=self.recovery
  for session,sequence in [(1,1),(0xdeadbeef,0xfffffffe)]:
   self.assertEqual(Frame.decode(recovery.close_frame(session,sequence)[:-1]),Frame(Channel.CONTROL,Kind.REQUEST,Op.CLOSE,session,1,sequence))
  payload=string('02:00:00:00:00:01')+string('a'*32)+struct.pack('<IB',12,0)
  encoded=Frame(0,3,8,77,0,1,payload).encode();d=recovery.RecoveryDecoder()
  self.assertEqual(list(d.receive(b'x'*20000+b'\0'+encoded)),[recovery.decode(encoded[:-1])])
  self.assertEqual(recovery.notice(recovery.decode(encoded[:-1])),(77,12,b'a'*32))
  self.assertIsNone(recovery.notice((0,3,8,77,0,1,payload[:-1]+b'\1')))
  broken=bytearray(encoded);broken[5]^=2
  self.assertEqual(list(d.receive(broken)),[])
 def test_recovery_requires_close_ack_and_nonce_boundary(self):
  recovery=self.recovery;nonce=b'a'*32
  class Peer:
   def __init__(self):self.buffer=b'';self.written=[]
   def write(self,data):
    self.written.append(data)
    if data.startswith(b'\0'):self.buffer=Frame(0,2,3,77,1,1,b'\x20\0'+nonce).encode()
    elif data==b'host_connection cli '+nonce+b'\n':self.buffer=b'@HOST_CONNECTION CLI\n> '
    else:raise AssertionError('Unexpected write')
    return len(data)
   @property
   def in_waiting(self):return len(self.buffer)
   def read(self,n):part=self.buffer[:n];self.buffer=self.buffer[n:];return part
  peer=Peer();recovery.recover(peer,(77,12,nonce));self.assertEqual(len(peer.written),2)

class CaptureEndOrderingTests(unittest.TestCase):
 def test_corrupt_or_duplicate_end_does_not_close_connection_or_double_count(self):
  with tempfile.TemporaryDirectory() as d:
   sink=CaptureSink(d,3,4);sink.finish(b'broken');sink.finish(struct.pack('<IIIII',3,4,1,1,1))
   self.assertTrue(sink.running);self.assertEqual(sink.stats['invalid_end_records'],2)
   end=struct.pack('<IIIII',3,4,2,2,0);sink.finish(end);sink.finish(end);sink.close()
   self.assertEqual(sink.stats['sequence_gaps'],2)
 def test_data_end_before_control_start_ack_is_buffered_in_order(self):
  import server,queue
  b=server.Bridge();b.running=False;b.host_wake();b.thread.join(1)
  try:
   b.pending_capture=queue.Queue(maxsize=32)
   record=Frame(Channel.CAPTURE,Kind.DATA,Op.CAPTURE_RECORD,77,0,1,struct.pack('<IIIQHHbB',3,0,4,1,1,1,-50,1)+b'x')
   end=Frame(Channel.CAPTURE,Kind.EVENT,Op.CAPTURE_STOP,77,0,2,struct.pack('<IIIII',3,4,1,0,1))
   b.host_event(record);b.host_event(end);early=b.pending_capture;b.pending_capture=None
   b.capture_sink=CaptureSink(b.transfers.directory,3,4)
   while not early.empty():b.host_event(early.get_nowait())
   b.capture_sink.close();self.assertEqual(b.capture_sink.stats['written'],1);self.assertTrue(b.capture_sink.ended)
  finally:b.close()

class NegotiatedCreditTests(unittest.TestCase):
 def test_download_progresses_with_small_and_large_negotiated_windows(self):
  from types import SimpleNamespace
  from server import Bridge
  data=bytes(range(256))*20
  for window in [1024,2048,8192]:
   with self.subTest(window=window),tempfile.TemporaryDirectory() as directory:
    bridge=SimpleNamespace(file_records=None,file_error=None)
    class Peer:
     closed=False;session=77;limits={'device_id':'02:00:00:00:00:01'};sent=0
     def request(self,op,payload,channel=Channel.COMMAND,**kw):
      future=concurrent.futures.Future()
      if op==Op.FILE_START:result=struct.pack('<III',1,len(data),9)+string('/x')+struct.pack('<I',window)
      elif op==Op.FILE_CREDIT:
       id_,acked,credit=struct.unpack('<III',payload);assert self.sent-acked+credit<=window
       for offset in range(self.sent,min(self.sent+credit,len(data)),1024):
        chunk=data[offset:min(offset+1024,self.sent+credit,len(data))]
        bridge.file_records.put_nowait(Frame(Channel.FILE,Kind.DATA,Op.FILE_RECORD,77,0,1,struct.pack('<IIIH',1,offset,9,len(chunk))+chunk))
       self.sent=min(self.sent+credit,len(data));result=b''
       if self.sent==len(data):bridge.file_records.put_nowait(Frame(Channel.FILE,Kind.EVENT,Op.FILE_END,77,0,2,struct.pack('<II',1,len(data))+hashlib.sha256(data).digest()))
      else:raise AssertionError(op)
      future.set_result(result);return future
    bridge.host_client=Peer();task={'path':'/x','sink_path':str(Path(directory)/'sink')}
    manager=SimpleNamespace(_check=lambda t:None,_update=lambda t,**kw:t.update(kw))
    Bridge.host_download(bridge,task,manager)
    self.assertEqual(task['status'],'COMPLETED');self.assertEqual(Path(task['sink_path']).read_bytes(),data)

class VersionTests(unittest.TestCase):
 def payload(self,upstream='1.18.0',extension='1.0'):
  return b''.join(string(x) for x in (upstream+'-jmod'+extension,upstream,extension,'waveshare-c5-rgb-usb-4m6m-v1','Waveshare ESP32-C5-WIFI6-KIT-N32R8-U'))
 def test_independent_release_axes_and_bounded_version_result(self):
  for upstream,extension in [('1.18.0','1.0'),('1.19.0','1.0'),('2.0.0','2.3.1')]:
   v=HostConnectionClient.version_result(self.payload(upstream,extension))
   self.assertEqual(v['firmware'],upstream+'-jmod'+extension)
   self.assertEqual(v['extension_version'],extension)
  for payload in [self.payload()+b'x',self.payload('01.18.0'),self.payload(extension='01.0'),string('x'*65)+self.payload()]:
   self.assertRaises(ValueError,HostConnectionClient.version_result,payload)
 def test_cli_and_discovery_full_version_display(self):
  import server,json
  m=server.Model();m.log('Version: v1.18.0');m.log('Firmware Build: 1.18.0-jmod1.0')
  self.assertEqual(m.state['firmware'],'1.18.0-jmod1.0')
  m.log('@MARAUDER:'+json.dumps(dict(firmware='v1.19.0',firmware_version='1.19.0-jmod1.0',capabilities=[])))
  self.assertEqual(m.state['firmware'],'1.19.0-jmod1.0')
 def test_package_version_is_from_built_source_and_checked_against_image(self):
  import importlib.util
  path=FIRMWARE_ROOT/'tools/package_usb_ota.py'
  spec=importlib.util.spec_from_file_location('package_version_test',path);mod=importlib.util.module_from_spec(spec);spec.loader.exec_module(mod)
  import tempfile
  with tempfile.TemporaryDirectory() as temporary:
   header=Path(temporary)/'FirmwareVersion.h'
   header.write_text('#define MARAUDER_UPSTREAM_VERSION "1.18.0"\n#define MARAUDER_EXTENSION_VERSION "2.2.0"\n#define MARAUDER_BUILD_TARGET "fixture"\n')
   v=mod.version_metadata(header,b'1.18.0-jmod2.2.0\0')
   self.assertEqual(v['version'],'1.18.0-jmod2.2.0')
   self.assertRaises(ValueError,mod.version_metadata,header,b'1.18.0-wrong\0')

class RecoveryNoticeTests(unittest.TestCase):
 def test_active_notice_fails_pending_without_text_dispatch(self):
  text=[];c=HostConnectionClient(77,lambda:None,lambda *args:text.append(args))
  future=c.request(Op.PING,channel=Channel.CONTROL)
  c.limits=dict(device_id='02:00:00:00:00:01',endpoint=0)
  c.feed(Frame(Channel.CONTROL,Kind.EVENT,Op.RECOVERY_NOTICE,77,0,1,b'bad').encode());self.assertFalse(c.closed)
  payload=string(c.limits['device_id'])+string('a'*32)+struct.pack('<IB',1,0)
  c.feed(Frame(Channel.CONTROL,Kind.EVENT,Op.RECOVERY_NOTICE,77,0,2,payload).encode())
  self.assertTrue(c.closed);self.assertRaises(ConnectionError,future.result)
  self.assertEqual(text,[]);self.assertEqual(c.metrics['recovery_notices'],1)

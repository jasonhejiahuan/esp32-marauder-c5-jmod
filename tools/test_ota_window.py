import base64,json,unittest
from unittest.mock import patch
from ota_window import upload,upload_binary
class Peer:
 def __init__(self,wrong=False):self.ser=self;self.buf=b'';self.rx=b'';self.offset=0;self.writes=0;self.first_read_writes=None;self.wrong=wrong
 @property
 def in_waiting(self):return len(self.rx)
 def write(self,line):
  self.writes+=1;a=line.decode().split();assert int(a[4])==self.offset;self.offset+=len(base64.b64decode(a[6]));r={'tx':a[2],'ok':True,'offset':self.offset+(1 if self.wrong else 0)};self.rx+=b'@OTA:'+json.dumps(r).encode()+b'\n';return len(line)
 def read(self,n):
  if self.first_read_writes is None:self.first_read_writes=self.writes
  b=self.rx[:n];self.rx=self.rx[n:];return b
class WindowTests(unittest.TestCase):
 def test_pipeline_is_bounded_and_completes_short_tail(self):
  p=Peer();seen=[];r=upload(p,'test',b'a'*10003,4,seen.append);self.assertEqual(p.first_read_writes,4);self.assertEqual(seen[-1],10003);self.assertEqual(r['recovered_windows'],0)
 def test_wrong_offset_rejected(self):
  with self.assertRaisesRegex(RuntimeError,'offset mismatch'):upload(Peer(True),'test',b'a'*2048)
 def test_missing_ack_resumes_only_matching_transaction(self):
  class Silent(Peer):
   def write(self,line):super().write(line);self.rx=b'';return len(line)
   def request(self,*a,**kw):return {'active':True,'session':'test','offset':self.offset}
  ticks=iter(range(1000));p=Silent()
  with patch('ota_window.time.monotonic',side_effect=lambda:next(ticks)):
   self.assertEqual(upload(p,'test',b'a'*2048)['recovered_windows'],1)
 def test_wrong_session_never_resumes(self):
  class Silent(Peer):
   def write(self,line):super().write(line);self.rx=b'';return len(line)
   def request(self,*a,**kw):return {'active':True,'session':'other','offset':self.offset}
  ticks=iter(range(1000))
  with patch('ota_window.time.monotonic',side_effect=lambda:next(ticks)):
   with self.assertRaisesRegex(RuntimeError,'safely resume'):upload(Silent(),'test',b'a'*2048)
 def test_binary_payload_preserves_zero_and_newline_and_short_tail(self):
  class BinaryPeer:
   def __init__(self):self.ser=self;self.buf=b'';self.rx=b'';self.received=b''
   def request(self,op,args,**kw):
    self.args=args.split();self.tx='chunk'+self.args[1];return {'tx':self.tx,'ok':True,'code':'ready'}
   def write(self,b):
    import binascii
    self.assertions=(len(b)==int(self.args[3]) and binascii.crc32(b)==int(self.args[2]))
    assert self.assertions;self.received+=b;self.rx=b'@OTA:'+json.dumps({'tx':self.tx,'ok':True,'offset':len(self.received)}).encode()+b'\n';return len(b)
   @property
   def in_waiting(self):return len(self.rx)
   def read(self,n):b=self.rx[:n];self.rx=self.rx[n:];return b
  p=BinaryPeer();data=bytes(range(256))*40+b'last';seen=[]
  upload_binary(p,'test',data,8192,seen.append);self.assertEqual(p.received,data);self.assertEqual(seen,[8192,len(data)])
 def test_window_bounds(self):
  for n in [0,5]:
   with self.assertRaises(ValueError):upload(Peer(),'test',b'a',n)
if __name__=='__main__':unittest.main()

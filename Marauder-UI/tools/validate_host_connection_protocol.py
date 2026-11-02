import sys,time,secrets,re,json,struct,traceback
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import argparse
parser=argparse.ArgumentParser(description='Host_Connection real-device validation; may start a passive scan or create an isolated test directory.')
parser.add_argument('--run-hardware',action='store_true',required=True)
parser.add_argument('--control',required=True,help='Control serial port selected by the operator')
parser.add_argument('--data',required=True,help='Data serial port selected by the operator')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'hardware-validation')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
from serial_endpoint import EndpointSerial
from transport import HostConnectionClient
from host_connection import *
OUT=args.output;report={}
s=EndpointSerial(args.data,115200,timeout=0,write_timeout=0,exclusive=True)
def write(data):
 p=0;end=time.monotonic()+3
 while p<len(data):
  p+=s.write(data[p:p+128]);time.sleep(.001)
  if time.monotonic()>end:raise TimeoutError('write')
def line(command,pattern,seconds=3):
 write(command+b'\n');buf=b'';end=time.monotonic()+seconds
 while time.monotonic()<end:
  buf+=s.read(min(s.in_waiting,8192) or 1)
  if re.search(pattern,buf):return buf
  time.sleep(.002)
 raise TimeoutError((command,buf[-100:]))
def pump(c,f):
 end=time.monotonic()+4
 while not f.done() and time.monotonic()<end:c.pump(s);c.feed(s.read(min(s.in_waiting,8192) or 1));time.sleep(.001)
 return f.result(timeout=0)
def upgrade(window=1024,features=31):
 nonce=secrets.token_hex(16);buf=line(('host_connection upgrade 1 '+nonce).encode(),rb'@HOST_CONNECTION READY '+nonce.encode()+rb' (\d+)\n')
 session=int(re.search(rb'READY '+nonce.encode()+rb' (\d+)\n',buf)[1]);c=HostConnectionClient(session,lambda:None)
 return c,nonce

def notice(seconds=4):
 decoder=Decoder();end=time.monotonic()+seconds
 while time.monotonic()<end:
  for f in decoder.feed(s.read(min(s.in_waiting,8192) or 1)):
   if f.op==Op.RECOVERY_NOTICE:return f
  time.sleep(.002)
 raise TimeoutError('notice')
def restore(f):
 r=Reader(f.payload);device=r.string(17);nonce=r.string(32);seq,endpoint=r.unpack('IB');r.end();write(b'\0')
 c=HostConnectionClient(f.session,lambda:None);c.sequence=seq;r=Reader(pump(c,c.request(Op.CLOSE,channel=Channel.CONTROL)));n=r.string(32);assert n==nonce
 line(('host_connection cli '+nonce).encode(),rb'@HOST_CONNECTION CLI');return device
try:
 first=line(b'deviceinfo --machine hc_before',rb'@DEVICE:[^\n]+\n');report['before']=json.loads(next(x[8:] for x in first.splitlines() if x.startswith(b'@DEVICE:')))
 write(b'protocol');time.sleep(.1);partial=s.read(s.in_waiting);assert b'@MARAUDER:' not in partial
 complete=line(b'info --machine incremental',rb'@MARAUDER:[^\n]+\n');assert b'"tx":"incremental"' in complete;report['incremental_cli']=True
 c,nonce=upgrade();f=notice(4);assert f.session==c.session;report['handshake_timeout_recovered']=restore(f)
 c,nonce=upgrade();payload=string(nonce)+struct.pack('<HII',3072,1024,4);report['limited_hello']=c.accept_hello(pump(c,c.request(Op.HELLO,payload,Channel.CONTROL)))
 try:pump(c,c.request(Op.FILE_START,string('/eapol_6.pcap')+struct.pack('<I',0)));raise AssertionError('feature accepted')
 except RuntimeError as e:assert str(e)=='feature_not_negotiated';report['feature_rejected']=str(e)
 # Corrupt and oversize bytes include newline and a real CLI command. They must remain binary.
 write(b'\0'+b'reboot\n'*700+b'\0');pump(c,c.request(Op.PING,channel=Channel.CONTROL));report['malformed_keeps_session']=True
 reply=pump(c,c.request(Op.CLOSE,channel=Channel.CONTROL));report['close_without_boundary_recovered']=restore(notice())
 c,nonce=upgrade();c.accept_hello(pump(c,c.request(Op.HELLO,string(nonce)+struct.pack('<HII',3072,1024,31),Channel.CONTROL)))
 try:pump(c,c.request(Op.FILE_START,string('/eapol_6.pcap')+struct.pack('<I',2048)));raise AssertionError('negotiated window exceeded')
 except RuntimeError as e:assert str(e)=='payload';report['window_rejected']=str(e)
 r=Reader(pump(c,c.request(Op.CLOSE,channel=Channel.CONTROL)));nonce=r.string(32);line(('host_connection cli '+nonce).encode(),rb'@HOST_CONNECTION CLI')
 final=line(b'deviceinfo --machine hc_after',rb'@DEVICE:[^\n]+\n');report['after']=json.loads(next(x[8:] for x in final.splitlines() if x.startswith(b'@DEVICE:')))
 assert report['after']['uptime_ms']>report['before']['uptime_ms'];report['ok']=True
except Exception:report['error']=traceback.format_exc()
finally:s.close();(OUT/'protocol-hardware-validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2))

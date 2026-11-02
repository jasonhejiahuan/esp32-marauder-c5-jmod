import sys,time,json,traceback,concurrent.futures,hashlib
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import argparse
parser=argparse.ArgumentParser(description='Host_Connection real-device validation; may start a passive scan or create an isolated test directory.')
parser.add_argument('--run-hardware',action='store_true',required=True)
parser.add_argument('--control',required=True,help='Control serial port selected by the operator')
parser.add_argument('--data',required=True,help='Data serial port selected by the operator')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'hardware-validation')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
from server import Bridge
from transport import HostConnectionClient
from host_connection import *
OUT=args.output
b=Bridge();report={};prefix='/__Host_Connection_validation_'+str(int(time.time()))
def call(a,**kw):return b.call(a,**kw)
def wait(t):
 end=time.monotonic()+20
 while time.monotonic()<end:
  t=b.transfers.request('status',transfer_id=t['transfer_id'])
  if t['status'] in {'COMPLETED','FAILED','CANCELLED'}:return t
  time.sleep(.01)
 raise TimeoutError(t)
try:
 call('connect',port=args.control)
 # A genuinely negotiated data endpoint is rejected for an incorrect binding identity.
 request=HostConnectionClient.request
 def wrong(self,op,payload=b'',*args,**kw):
  if op==Op.BIND:payload=string('00:00:00:00:00:01')+payload[19:]
  return request(self,op,payload,*args,**kw)
 with patch.object(HostConnectionClient,'request',wrong):
  try:call('transport',operation='bind',port=args.data);raise AssertionError('wrong identity accepted')
  except RuntimeError as e:assert 'binding_rejected' in str(e);report['wrong_identity']=str(e)
 b.host_client.request(Op.PING,channel=Channel.CONTROL).result(2)
 old_token=b.host_client.request(Op.BIND_TOKEN,channel=Channel.CONTROL).result(2);time.sleep(5.2)
 def expired(self,op,payload=b'',*args,**kw):
  if op==Op.BIND_TOKEN:
   f=concurrent.futures.Future();f.set_result(old_token);return f
  return request(self,op,payload,*args,**kw)
 with patch.object(HostConnectionClient,'request',expired):
  try:call('transport',operation='bind',port=args.data);raise AssertionError('expired token accepted')
  except RuntimeError as e:assert 'binding_rejected' in str(e);report['expired_token']=str(e)
 report['dual']=call('transport',operation='bind',port=args.data)
 try:call('transport',operation='bind',port=args.data);raise AssertionError('duplicate bind')
 except RuntimeError as e:report['duplicate_bind']=str(e)
 # Only own empty directories are mutated; existing captures remain untouched.
 for op,path,dest in [('mkdir',prefix,None),('rename',prefix,prefix+'_renamed'),('trash',prefix+'_renamed',None),('restore','/.trash'+prefix+'_renamed',None),('trash',prefix+'_renamed',None)]:
  report.setdefault('mutations',[]).append(call('files',operation=op,path=path,destination=dest))
 for op,path in [('trash','/'),('rename','/.trash')]:
  try:b.host_client.file_operation(op,path,'/not_created').result(2);raise AssertionError('protected path accepted')
  except RuntimeError as e:assert 'protected_path' in str(e);report.setdefault('protected_paths',[]).append(str(e))
 entries=call('files',operation='list',path='/')['entries'];files=[x for x in entries if not x['directory']]
 source=max(files,key=lambda x:x['size']);report['source']=source
 t=b.transfers.request('start',source['path']);deadline=time.monotonic()+3
 while b.transfers.request('status',transfer_id=t['transfer_id'])['offset']<4096 and time.monotonic()<deadline:time.sleep(.005)
 assert b.transfers.request('status',transfer_id=t['transfer_id'])['offset']>=4096
 report['cancelling']=b.transfers.request('cancel',transfer_id=t['transfer_id']);report['cancelled']=wait(t);assert report['cancelled']['status']=='CANCELLED'
 t0=time.monotonic();t=b.transfers.request('start',source['path']);report['download']=wait(t);report['download_seconds']=time.monotonic()-t0;assert report['download']['status']=='COMPLETED'
 f,size,name=b.transfers.download(t['transfer_id']);sha=hashlib.sha256()
 while chunk:=f.read(65536):sha.update(chunk)
 f.close();assert sha.hexdigest()==report['download']['sha256'];report['sink_sha256']=sha.hexdigest()
 call('disconnect');call('connect',port=args.data,mode='LEGACY_CLI');report['legacy_stat']=call('files',operation='stat',path=source['path']);assert report['legacy_stat']['size']==source['size'];call('disconnect');report['ok']=True
except Exception:report['error']=traceback.format_exc()
finally:
 try:call('disconnect')
 except Exception as e:report['close_error']=str(e)
 b.running=False;b.host_wake();b.thread.join(2);b.close();(OUT/'file-binding-validation.json').write_text(json.dumps(report,indent=2));print(json.dumps(report,indent=2),flush=True)

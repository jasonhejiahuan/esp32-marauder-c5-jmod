import sys,time,json,traceback,statistics
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import argparse
parser=argparse.ArgumentParser(description='Host_Connection real-device validation; may start a passive scan or create an isolated test directory.')
parser.add_argument('--run-hardware',action='store_true',required=True)
parser.add_argument('--control',required=True,help='Control serial port selected by the operator')
parser.add_argument('--data',required=True,help='Data serial port selected by the operator')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'hardware-validation')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
from server import Bridge
from host_connection import Op,Channel
out=args.output
b=Bridge();report={}
def call(action,**kw):
 r=b.call(action,**kw);print(action,r,flush=True);return r
def stats():
 f=b.host_client.request(Op.STATUS,channel=Channel.CONTROL);f.result(4);b.host_stats(f);return b.model.state['host_connection_stats']
try:
 call('connect',port=args.control);report['negotiation']=b.host_client.limits
 call('transport',operation='bind',port=args.data)
 call('command',command='scancombined',duration=60);time.sleep(2);call('capture',operation='start')
 latency=[]
 for i in range(60):
  t=time.monotonic();b.host_client.request(Op.PING,channel=Channel.CONTROL).result(2);latency.append((time.monotonic()-t)*1000);time.sleep(.08)
 report['capture']=b.capture_sink.snapshot();report['device']=stats();report['rtt_ms']=latency
 print('capture',report['capture'],flush=True)
 # Deliberately revoke data descriptor to test containment. Sole normal I/O owner remains Bridge.
 b.data_ser.close();time.sleep(.5);report['data_failure_mode']=b.model.state['transport']
 t=time.monotonic();b.host_client.request(Op.PING,channel=Channel.CONTROL).result(3);report['control_after_failure_ms']=(time.monotonic()-t)*1000
 print('control survived',report['data_failure_mode'],flush=True)
 time.sleep(13)
 report['fallback']=call('transport',operation='fallback');report['fallback_status']=stats()
 report['rebind']=call('transport',operation='bind',port=args.data)
 call('command',command='stopscan');call('disconnect');report['ok']=True
except Exception:report['error']=traceback.format_exc();print(report['error'],flush=True)
finally:
 try:call('disconnect')
 except Exception as e:report['close_error']=str(e)
 b.running=False;b.host_wake();b.thread.join(2);b.close();(out/'dual-isolation-verified.json').write_text(json.dumps(report,indent=2))

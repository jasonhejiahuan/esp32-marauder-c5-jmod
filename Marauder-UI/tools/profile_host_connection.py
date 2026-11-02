import sys,time,json,threading,statistics,socket,traceback
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import argparse
parser=argparse.ArgumentParser(description='Host_Connection real-device validation; may start a passive scan or create an isolated test directory.')
parser.add_argument('--run-hardware',action='store_true',required=True)
parser.add_argument('--control',required=True,help='Control serial port selected by the operator')
parser.add_argument('--data',required=True,help='Data serial port selected by the operator')
parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'hardware-validation')
args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=True)
from server import Server,Handler,Bridge
from host_connection import Op,Channel
OUT=args.output
srv=Server(('127.0.0.1',8766),Handler);srv.token='HostConnectionLocalValidation';srv.bridge=Bridge();b=srv.bridge
threading.Thread(target=srv.serve_forever,daemon=True).start();signal=Path('/tmp/hc-profile-stage');signal.write_text('starting');report={}
def stats():
 f=b.host_client.request(Op.STATUS,channel=Channel.CONTROL);f.result(4);b.host_stats(f);return b.model.state['host_connection_stats'].copy()
def wait_for(value):
 signal.write_text('waiting_'+value);print('WAIT',value,flush=True);end=time.monotonic()+600
 while signal.read_text()!=value:
  if time.monotonic()>end:raise TimeoutError(value)
  time.sleep(.05)
def phase(label,slow=False):
 b.call('capture',operation='start');sink=b.capture_sink;slow_socket=None
 if slow:
  slow_socket=socket.create_connection(('127.0.0.1',8766));slow_socket.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,256)
  slow_socket.sendall(f'GET /api/capture/stream/{sink.id} HTTP/1.1\r\nHost: 127.0.0.1:8766\r\n\r\n'.encode())
 samples=[];begin=stats();start=time.monotonic()
 while time.monotonic()-start<10:
  at=time.monotonic();b.host_client.request(Op.PING,channel=Channel.CONTROL).result(3);samples.append((time.monotonic()-at)*1000);time.sleep(.07)
 end=stats();b.call('capture',operation='stop');sink.close()
 if slow_socket:slow_socket.close()
 if b.model.state.get('transport')!='DUAL_LINK':raise RuntimeError('Unexpected data link failure: '+str(b.model.state.get('data_link_diagnostics')))
 samples.sort();result=dict(seconds=time.monotonic()-start,count=len(samples),rtt_ms=dict(p50=statistics.median(samples),p95=samples[int(len(samples)*.95)],p99=samples[int(len(samples)*.99)],max=max(samples)),device=end,capture=sink.snapshot())
 report[label]=result;print(label,json.dumps(result),flush=True);(OUT/'ui-load-profile.json').write_text(json.dumps(report,indent=2))
try:
 b.call('connect',port=args.control);b.call('transport',operation='bind',port=args.data);b.call('command',command='scancombined',duration=500);time.sleep(2)
 phase('ui_closed');wait_for('open');phase('ui_open');wait_for('reload');phase('ui_reloading');wait_for('slow');phase('slow_live_client',slow=True)
 b.call('command',command='stopscan');
 # Idle dual-link session refreshes must survive without capture traffic kicking USB TX.
 deadline=time.monotonic()+120
 while time.monotonic()<deadline:
  b.host_client.request(Op.PING,channel=Channel.CONTROL).result(3);time.sleep(.2)
  if b.model.state.get('transport')!='DUAL_LINK':raise RuntimeError('Idle data link failed: '+str(b.model.state.get('data_link_diagnostics')))
 f=b.host_client.request(Op.LINK_DIAGNOSTICS,channel=Channel.CONTROL);f.result(4);b.host_link_diagnostics(f)
 report['idle_dual_120s']=True;report['link_diagnostics']=b.model.state.get('device_link_diagnostics');signal.write_text('files_ready');print('FILES READY',flush=True)
 wait_for('finish')
except Exception:
 report['error']=traceback.format_exc()
 try:
  f=b.host_client.request(Op.LINK_DIAGNOSTICS,channel=Channel.CONTROL);f.result(4);b.host_link_diagnostics(f)
 except Exception:pass
 report['state']=b.model.state.copy();print(report['error'],flush=True)
finally:
 try:b.call('disconnect')
 except Exception:pass
 b.running=False;b.host_wake();b.thread.join(2);b.close();srv.shutdown();srv.server_close();(OUT/'ui-load-profile.json').write_text(json.dumps(report,indent=2))

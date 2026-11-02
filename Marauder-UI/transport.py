"""Transport-neutral clients. Host_Connection pending requests never own serial reads.

Bridge is the sole I/O owner. Any backend worker may submit a bounded request;
only Bridge.pump() drains TX and feeds RX. Timeouts on mutations are indeterminate.
"""
import collections
import concurrent.futures
import dataclasses
import secrets
import struct
import threading
import time
from host_connection import Channel as C, Kind as K, Op, Frame, Decoder, Reader, string, MAX_PAYLOAD

class IndeterminateError(TimeoutError): pass

class LegacyCliClient:
    mode='LEGACY_CLI'
    def __init__(self,bridge): self.bridge=bridge
    def command(self,line,pattern,timeout=4): return self.bridge._legacy_exchange(line,pattern,timeout)
    def files(self,**kw): return self.bridge.files(**kw)

@dataclasses.dataclass
class Pending:
    future:concurrent.futures.Future
    op:int
    deadline:float
    mutation:bool
    started:float
    channel:int

class HostConnectionClient:
    mode='SINGLE_LINK'
    def __init__(self,session,wake,on_console=lambda b,r:None,on_event=lambda f:None):
        self.session=session;self.wake=wake;self.on_console=on_console;self.on_event=on_event
        self.decoder=Decoder();self.lock=threading.RLock();self.pending={}
        self.tx=collections.deque();self.urgent=collections.deque();self.tx_bytes=0;self.partial=None;self.offset=0
        self.next_request=1;self.sequence=0;self.rx_sequence=0;self.last_tx=time.monotonic()
        self.closed=False;self.close_reason=None;self.limits={};self.latencies=collections.deque(maxlen=512)
        self.metrics=collections.Counter();self.data=False
    def request(self,op,payload=b'',channel=C.COMMAND,timeout=5,mutation=False):
        with self.lock:
            if self.closed: raise ConnectionError('Host_Connection closed')
            urgent=channel==C.CONTROL and op in {Op.PING,Op.CLOSE,Op.FILE_CANCEL,Op.CAPTURE_STOP,Op.HELLO}
            if len(self.pending)>=(32 if urgent else 24): raise BufferError('Host_Connection pending limit')
            if self.next_request>=0xffffffff or self.sequence>=0xffffffff:
                raise ConnectionError('Host_Connection counter exhausted; reconnect')
            if len(payload)>MAX_PAYLOAD:raise ValueError('Host_Connection payload limit')
            req=self.next_request;self.next_request+=1
            frame=Frame(channel,K.REQUEST,op,self.session,req,0,payload)
            reserved=24+len(payload)+(24+len(payload))//254+2
            if self.tx_bytes+reserved>(12288 if urgent else 8192): raise BufferError('Host_Connection TX byte limit')
            future=concurrent.futures.Future();now=time.monotonic()
            self.pending[req]=Pending(future,op,now+timeout,mutation,now,channel)
            (self.urgent if urgent else self.tx).append((frame,reserved));self.tx_bytes+=reserved
        self.wake();return future
    def hello(self,nonce,features=None):
        payload=string(nonce)+struct.pack('<HI',MAX_PAYLOAD,8192)
        if features is not None:payload+=struct.pack('<I',features&31)
        return self.request(Op.HELLO,payload,C.CONTROL)
    def accept_hello(self,payload):
        r=Reader(payload);device=r.string(17);size,window,lease,endpoint,role,epoch=r.unpack('HIIBBI')
        features=31;operations=[]
        if r.offset<len(r.data):
            features,count=r.unpack('IB')
            if count>32 or features&~31:raise ValueError('Host_Connection capability bounds')
            operations=list(r.unpack('H'*count))
        r.end()
        if size!=MAX_PAYLOAD or not 1024<=window<=8192 or not 3000<=lease<=30000 or endpoint>1 or role>1:
            raise ValueError('Host_Connection negotiation limits')
        self.data=bool(role);self.limits=dict(device_id=device,max_payload=size,window=window,lease_ms=lease,endpoint=endpoint,role=role,epoch=epoch,features=features,operations=operations)
        return self.limits
    @staticmethod
    def version_result(payload):
        import re
        r=Reader(payload)
        version,upstream,extension,profile,target=[r.string(n) for n in (64,32,32,64,96)];r.end()
        numeric=r"(?:0|[1-9][0-9]*)"
        if not re.fullmatch(numeric+r"\."+numeric+r"\."+numeric,upstream) or not re.fullmatch(numeric+r"\."+numeric+r"(?:\."+numeric+r")?",extension) or version!=upstream+"-jmod"+extension:
            raise ValueError("Host_Connection invalid firmware version")
        return dict(firmware=version,upstream_version=upstream,extension_version=extension,build_profile=profile,build_target=target)
    def feed(self,data):
        for frame in self.decoder.feed(data):
            if frame.session!=self.session or frame.sequence<=self.rx_sequence:
                self.metrics['stale_frames']+=1;continue
            self.rx_sequence=frame.sequence
            if frame.kind in {K.RESULT,K.ERROR}:
                with self.lock:pending=self.pending.pop(frame.request,None)
                if not pending: self.metrics['late_results']+=1;continue
                if pending.future.done():continue
                if frame.op!=pending.op or frame.channel!=pending.channel:
                    pending.future.set_exception(ValueError('Host_Connection response operation mismatch'));continue
                self.latencies.append(time.monotonic()-pending.started)
                if frame.kind==K.ERROR:
                    try:r=Reader(frame.payload);error=r.string();r.end()
                    except ValueError:error='invalid error payload'
                    pending.future.set_exception(IndeterminateError(error) if error=='indeterminate_request' else RuntimeError(error))
                else:pending.future.set_result(frame.payload)
            elif frame.channel==C.CONTROL and frame.kind==K.EVENT and frame.op==Op.RECOVERY_NOTICE:
                # Explicit device session transition; do not wait for a later PING timeout.
                try:
                    r=Reader(frame.payload);device=r.string(17);nonce=r.string(32);seq,endpoint=r.unpack('IB');r.end()
                    if device!=self.limits.get('device_id') or len(nonce)!=32 or any(x not in '0123456789abcdefABCDEF' for x in nonce) or endpoint!=self.limits.get('endpoint'):raise ValueError('notice identity')
                except ValueError:
                    self.metrics['invalid_recovery_notices']+=1;continue
                self.metrics['recovery_notices']+=1
                self.close('Host_Connection device entered recovery')
            elif frame.channel==C.CONSOLE and frame.op==Op.CONSOLE:
                self.on_console(frame.payload,frame.request)
            else:self.on_event(frame)
    def pump(self,serial):
        now=time.monotonic();submitted=0
        with self.lock:
            for key,pending in list(self.pending.items()):
                if now>=pending.deadline:
                    self.pending.pop(key,None)
                    if not pending.future.done():pending.future.set_exception((IndeterminateError if pending.mutation else TimeoutError)('Host_Connection response timeout; completion unknown' if pending.mutation else 'Host_Connection response timeout'))
            if self.closed:return
            while self.partial is None and (self.urgent or self.tx):
                frame,reserved=(self.urgent or self.tx).popleft();self.tx_bytes-=reserved
                if frame.request not in self.pending:continue
                self.sequence+=1
                self.partial=dataclasses.replace(frame,sequence=self.sequence).encode();self.offset=0;self.tx_bytes+=len(self.partial)
            if self.partial is not None:
                remaining=self.partial[self.offset:self.offset+256]
                n=serial.write(remaining)
                if not 0<=n<=len(remaining):raise IOError('invalid serial write count')
                if n<len(remaining):self.metrics['partial_writes']+=1
                self.offset+=n;self.tx_bytes-=n;self.last_tx=now;submitted=n;self.metrics["tx_bytes"]+=n
                if self.offset==len(self.partial):self.partial=None;self.offset=0
            session_refresh=not self.tx_bytes and now-self.last_tx>2 and not any(p.op==Op.PING for p in self.pending.values())
        if session_refresh:
            f=self.request(Op.PING,channel=C.CONTROL,timeout=4)
            def session_refresh_done(f):
                if f.exception():self.metrics.update(session_refresh_failures=1);self.close('Host_Connection lease session_refresh failed')
            f.add_done_callback(session_refresh_done)
        return submitted
    def close(self,error='Host_Connection disconnected'):
        with self.lock:
            if self.closed:return
            self.closed=True;self.close_reason=str(error)
            pending=list(self.pending.values());self.pending.clear();self.tx.clear();self.urgent.clear();self.partial=None;self.tx_bytes=0
            for p in pending:
                if not p.future.done():p.future.set_exception(IndeterminateError(error) if p.mutation else ConnectionError(error))
    def command(self,line,pattern=None,timeout=8):return self.cli(line,timeout)
    def cli(self,line,timeout=8):
        return self.request(Op.CLI_EXEC,string(line),timeout=timeout,mutation=True)
    def file_operation(self,operation,path,destination='',offset=0,length=1024):
        ops={'list':1,'stat':2,'read':3,'rename':4,'mkdir':5,'trash':6,'restore':7,'delete':8,'emptytrash':9}
        op=ops[operation]
        return self.request(Op.FILE_OP,bytes([op])+string(path)+string(destination or '')+struct.pack('<II',offset,length),mutation=op>=4,timeout=14 if op>=8 else 5)
    @staticmethod
    def file_result(payload,path):
        r=Reader(payload);op,size,offset,next_,total,used,count=r.unpack('BIIiIIH');destination=r.string(160);n,=r.unpack('H');data=r.take(n);entries=[]
        if count>20 or n>1024:raise ValueError('file result bounds')
        for _ in range(count):
            name=r.string(160);directory,size_=r.unpack('BI')
            if not name or '/' in name or '\\' in name or directory>1:raise ValueError('invalid entry')
            entries.append(dict(name=name,path=path.rstrip('/')+'/'+name,directory=bool(directory),size=size_))
        r.end()
        return dict(ok=True,size=size,offset=offset,next=next_,total=total,used=used,entries=entries,path=destination,raw=data)

"""Independent bounded PCAP sink. USB readers only enqueue complete binary records."""
import collections
import os
import queue
import secrets
import struct
import tempfile
import threading
import time
from pathlib import Path
from host_connection import Reader

class CaptureSink:
    def __init__(self,directory,stream,epoch,snaplen=2324,writer=None):
        self.host_only=False;self.id=secrets.token_hex(16);self.stream=stream;self.epoch=epoch;self.snaplen=snaplen
        self.path=Path(directory)/(self.id+'.pcap');self.queue=queue.Queue(maxsize=128)
        self.stats=collections.Counter();self.lock=threading.Lock();self.running=True;self.last_sequence=None;self.ended=False
        self.writer=writer;self.error=None;self.started=time.time();self.max_bytes=256*1024*1024
        self.subscribers=();self.subscription_lock=threading.Lock()
        self.thread=threading.Thread(target=self._run,daemon=True,name='Host_Connection-PCAP');self.thread.start()
    def accept(self,payload):
        try:
            r=Reader(payload);stream,seq,epoch,timestamp,original,captured,rssi,channel=r.unpack('IIIQHHbB')
            if stream!=self.stream or epoch!=self.epoch or captured>original or captured>self.snaplen:raise ValueError('record identity/length')
            data=r.take(captured);r.end()
            if self.last_sequence is not None:
                if seq<=self.last_sequence:raise ValueError('capture sequence reuse')
                self.stats['sequence_gaps']+=seq-self.last_sequence-1
            else:self.stats['sequence_gaps']+=seq
            self.last_sequence=seq;self.stats['received']+=1
            # timestamp is microseconds since device boot, not wall time. PCAP is valid
            # with this relative epoch; do not subtract host monotonic/device clocks.
            record=struct.pack('<IIII',timestamp//1000000,timestamp%1000000,captured,original)+data
            if not self.running:self.stats['sink_drops']+=1;return
            for subscriber in self.subscribers:
                try:subscriber.put_nowait(record)
                except queue.Full:self.stats['subscriber_drops']+=1
            try:self.queue.put_nowait((time.monotonic(),record))
            except queue.Full:self.stats['backend_drops']+=1
        except (ValueError,struct.error):self.stats['invalid_records']+=1
    def _run(self):
        file=None
        try:
            file=self.writer or open(self.path,'xb',buffering=0)
            self._write(file,self.header())
            while self.running or not self.queue.empty():
                try:at,record=self.queue.get(timeout=.1)
                except queue.Empty:continue
                if time.monotonic()-at>1:self.stats['sink_drops']+=1;continue
                if self.stats['bytes']+len(record)>self.max_bytes:raise OSError('capture sink size limit')
                self._write(file,record);self.stats['written']+=1;self.stats['bytes']+=len(record)
        except Exception as e:
            self.error=str(e);self.running=False
            while True:
                try:self.queue.get_nowait();self.stats['sink_drops']+=1
                except queue.Empty:break
        finally:
            if file and not self.writer:file.close()
    @staticmethod
    def _write(file,data):
        view=memoryview(data)
        while view:
            n=file.write(view)
            if not n:raise OSError('capture sink short write')
            view=view[n:]
    def snapshot(self):
        return dict(host_only=self.host_only,capture_id=self.id,stream=self.stream,epoch=self.epoch,active=self.running,
                    path=str(self.path),error=self.error,queue=self.queue.qsize(),**dict(self.stats))
    def finish(self,payload):
        try:
            r=Reader(payload);stream,epoch,produced,dropped,submitted=r.unpack('IIIII');r.end()
            if (stream,epoch)!=(self.stream,self.epoch) or produced!=dropped+submitted or submitted<self.stats['received'] or produced<(0 if self.last_sequence is None else self.last_sequence+1):raise ValueError('Capture end identity/counts')
        except (ValueError,struct.error):self.stats['invalid_end_records']+=1;return
        if self.ended:return
        self.ended=True
        final_gap=max(0,produced-(0 if self.last_sequence is None else self.last_sequence+1))
        self.stats['sequence_gaps']+=final_gap
        self.stats.update(device_produced=produced,device_queue_drops=dropped,device_submitted=submitted,
                          transport_missing_records=max(0,submitted-self.stats['received']))
        self.close(wait=False)
    def header(self):return struct.pack('<IHHIIII',0xa1b2c3d4,2,4,0,0,self.snaplen,105)
    def subscribe(self):
        with self.subscription_lock:
            if not self.running:raise ValueError('Capture is not active')
            if len(self.subscribers)>=4:raise ValueError('Capture subscriber limit')
            subscriber=queue.Queue(maxsize=128);self.subscribers=(*self.subscribers,subscriber);return subscriber
    def unsubscribe(self,subscriber):
        with self.subscription_lock:self.subscribers=tuple(x for x in self.subscribers if x is not subscriber)
    def close(self,wait=True):
        self.running=False
        if wait:self.thread.join(2)
        if wait and self.thread.is_alive():self.error='sink stalled; isolated from USB reader'

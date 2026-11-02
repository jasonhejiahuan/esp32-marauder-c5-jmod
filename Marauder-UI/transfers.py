"""Backend-owned, bounded file downloads; no dependency on browser lifetime."""
import base64
import hashlib
import os
import secrets
import re
import tempfile
import threading
import time
from pathlib import Path

TERMINAL = {'COMPLETED', 'CANCELLED', 'FAILED'}
MAX_FILE = 6 * 1024 * 1024

class Cancelled(Exception): pass

class VerifiedSink:
    """Sequential records only: reject gaps, duplicates, overlaps and changing identity."""
    def __init__(self, path, size, identity):
        if type(size) is not int or not 0 <= size <= MAX_FILE:
            raise ValueError('Invalid expected size')
        self.size, self.identity, self.offset = size, identity, 0
        self.digest = hashlib.sha256()
        self.file = open(path, 'xb', buffering=0)
    def record(self, identity, offset, data):
        if identity != self.identity or offset != self.offset or not data or offset + len(data) > self.size:
            raise ValueError('Missing, duplicate, overlapping or inconsistent file record')
        if self.file.write(data) != len(data): raise OSError('Short sink write')
        self.digest.update(data); self.offset += len(data)
    def complete(self, size, digest):
        if size != self.size or self.offset != self.size or digest != self.digest.digest():
            raise ValueError('Whole-file SHA-256/byte-count mismatch')
        self.file.flush(); os.fsync(self.file.fileno())
        return self.digest.hexdigest()
    def close(self): self.file.close()

class TransferManager:
    def __init__(self, bridge, directory=None):
        self.bridge = bridge; self.lock = threading.RLock(); self.items = {}
        self.directory = Path(directory or tempfile.mkdtemp(prefix='marauder-host-connection-'))
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)
    def _public(self, t):
        return {k:v for k,v in t.items() if k not in {'cancel','worker','sink_path'}}
    def request(self, operation, path=None, transfer_id=None):
        with self.lock:
            if operation == 'list': return {'transfers':[self._public(t) for t in self.items.values()]}
            if operation == 'start':
                self.bridge.file_path(path)
                if transfer_id is not None:
                    if not isinstance(transfer_id,str) or not re.fullmatch(r'[0-9a-f]{32}',transfer_id):
                        raise ValueError('传输 ID 无效')
                    previous=self.items.get(transfer_id)
                    if previous:
                        if previous['path']!=path:raise ValueError('传输 ID 已用于其他文件')
                        return self._public(previous)
                if any(t['status'] not in TERMINAL for t in self.items.values()):
                    raise ValueError('已有文件传输正在运行')
                # Retention is bounded by count and by MAX_FILE per item; never remove an active sink.
                while len(self.items) >= 16:
                    old = next(k for k,v in self.items.items() if v['status'] in TERMINAL)
                    self._discard(self.items.pop(old))
                key = transfer_id or secrets.token_hex(16)
                t = dict(transfer_id=key, path=path, source_identity=None, expected_size=None,
                         offset=0, progress=0, status='CREATED', error=None, sha256=None,
                         created=time.time(), cancel=threading.Event(), sink_path=str(self.directory/key))
                self.items[key] = t
                t['worker'] = threading.Thread(target=self._run, args=(t,), daemon=True,
                                               name='Host_Connection-transfer')
                t['worker'].start()
                return self._public(t)
            t = self.items.get(transfer_id)
            if not t: raise ValueError('传输不存在或已过期')
            if operation == 'cancel' and t['status'] not in TERMINAL:
                t['status']='CANCELLING'; t['cancel'].set()
            elif operation not in {'cancel','status'}: raise ValueError('传输操作无效')
            return self._public(t)
    def _check(self,t):
        if t['cancel'].is_set(): raise Cancelled()
    def _update(self,t,**kw):
        with self.lock: t.update(kw)
    def _discard(self,t):
        Path(t['sink_path']).unlink(missing_ok=True)
    def _run(self,t):
        sink=None
        try:
            self._check(t); self._update(t,status='RUNNING')
            # Transport selection belongs to the backend, not the HTTP/browser layer.
            if getattr(self.bridge,'host_client',None):
                self.bridge.host_download(t, self)
                return
            meta=self.bridge.call('files',operation='stat',path=t['path'])
            size=meta['size']; identity=(self.bridge.model.instance,self.bridge.model.state.get('port'),t['path'],size)
            self._update(t,expected_size=size,source_identity=list(identity))
            sink=VerifiedSink(t['sink_path'],size,identity)
            while sink.offset<size:
                self._check(t)
                r=self.bridge.call('files',operation='read',path=t['path'],offset=sink.offset,length=1024)
                if r['size']!=size: raise ValueError('文件已变化')
                sink.record(identity,r['offset'],base64.b64decode(r['data'],validate=True))
                self._update(t,offset=sink.offset,progress=sink.offset/max(1,size))
            # Older firmware has only per-chunk CRC. A second bounded pass verifies the
            # complete byte stream; never label the first pass SHA as device verification.
            verify=hashlib.sha256(); offset=0
            while offset<size:
                self._check(t)
                r=self.bridge.call('files',operation='read',path=t['path'],offset=offset,length=1024)
                data=base64.b64decode(r['data'],validate=True)
                if r['size']!=size or r['offset']!=offset or not data or offset+len(data)>size:
                    raise ValueError('文件在验证期间变化')
                verify.update(data); offset+=len(data)
            self._check(t)
            final=self.bridge.call('files',operation='stat',path=t['path'])
            digest=sink.complete(final['size'],verify.digest())
            self._update(t,status='COMPLETED',progress=1,sha256=digest,verification='legacy-two-pass-sha256')
        except Cancelled: self._update(t,status='CANCELLED')
        except Exception as e: self._update(t,status='FAILED',error=str(e))
        finally:
            if sink: sink.close()
            if t['status']!='COMPLETED': self._discard(t)
    def download(self,key):
        with self.lock:
            t=self.items.get(key)
            if not t or t['status']!='COMPLETED': raise ValueError('文件传输尚未完成')
            return open(t['sink_path'],'rb'), t['expected_size'], Path(t['path']).name
    def close(self):
        with self.lock:
            for t in self.items.values():
                if t['status'] not in TERMINAL: t['cancel'].set()
        for t in list(self.items.values()):
            t['worker'].join(2)
            if not t['worker'].is_alive(): self._discard(t)

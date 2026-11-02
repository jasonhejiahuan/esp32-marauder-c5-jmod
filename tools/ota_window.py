"""Bounded USB OTA write window; only correlated acknowledgements advance progress."""
import base64,binascii,json,secrets,time

def upload(link,session,data,window=4,progress=lambda offset:None):
    if not 1<=window<=4:raise ValueError('OTA window out of range')
    confirmed=0;sent=0;pending={};deadline=time.monotonic()+3;recoveries=0
    while confirmed<len(data):
        while len(pending)<window and sent<len(data):
            chunk=data[sent:sent+1024];tx='win_'+secrets.token_hex(6)
            line=f'ota write {tx} {session} {sent} {binascii.crc32(chunk)} '+base64.b64encode(chunk).decode()+'\n'
            raw=line.encode();written=link.ser.write(raw)
            if written!=len(raw):raise IOError('Incomplete OTA command write; current boot slot preserved')
            pending[tx]=(sent,sent+len(chunk));sent+=len(chunk)
        incoming=link.ser.read(min(link.ser.in_waiting,4096) or 1);link.buf+=incoming
        while b'\n' in link.buf:
            line,link.buf=link.buf.split(b'\n',1);line=line.strip()
            if not line.startswith(b'@OTA:'):continue
            try:r=json.loads(line[5:])
            except ValueError:continue
            bounds=pending.get(r.get('tx'))
            if bounds is None:continue
            if not r.get('ok'):raise RuntimeError('Device rejected OTA chunk: '+str(r.get('code')))
            if r.get('offset')!=bounds[1]:raise RuntimeError('OTA acknowledgement offset mismatch')
            # Serial responses follow flash write order. Never commit a gap.
            if bounds[0]!=confirmed:raise RuntimeError('OTA acknowledgement gap')
            del pending[r['tx']];confirmed=bounds[1];progress(confirmed);deadline=time.monotonic()+3
        if len(link.buf)>16384:raise RuntimeError('OTA response buffer exceeded limit')
        if time.monotonic()>deadline:
            # status is a queue barrier after every transmitted chunk. Resume only
            # an explicitly matching active transaction and a valid chunk boundary.
            if recoveries>=2:raise TimeoutError('OTA acknowledgement timed out')
            status=link.request('status',timeout=3,attempts=1)
            offset=status.get('offset',-1)
            if not status.get('active') or status.get('session')!=session or not confirmed<=offset<=sent or (offset%1024 and offset!=len(data)):
                raise RuntimeError('Cannot safely resume OTA window')
            confirmed=sent=offset;pending.clear();recoveries+=1;deadline=time.monotonic()+3;progress(confirmed)
    return {'window':window,'recovered_windows':recoveries}

def upload_binary(link,session,data,chunk_size=8192,progress=lambda offset:None):
    if not 1024<=chunk_size<=8192:raise ValueError('Invalid binary OTA chunk size')
    retries=0
    for offset in range(0,len(data),chunk_size):
        chunk=data[offset:offset+chunk_size];arguments=f'{session} {offset} {binascii.crc32(chunk)} {len(chunk)}'
        for attempt in range(3):
            try:
                ready=link.request('writebin',arguments,timeout=4,attempts=1)
                if ready.get('code')!='ready':raise RuntimeError('Missing binary OTA readiness confirmation')
                tx=ready['tx']
                if link.ser.write(chunk)!=len(chunk):raise OSError('Partial binary OTA write')
                deadline=time.monotonic()+4;reply=None
                while time.monotonic()<deadline and reply is None:
                    link.buf+=link.ser.read(min(link.ser.in_waiting,4096) or 1)
                    while b'\n' in link.buf:
                        line,link.buf=link.buf.split(b'\n',1);line=line.strip()
                        if not line.startswith(b'@OTA:'):continue
                        try:r=json.loads(line[5:])
                        except ValueError:continue
                        if r.get('tx')==tx:reply=r;break
                    if len(link.buf)>16384:raise RuntimeError('Binary OTA response overflow')
                if reply is None:raise TimeoutError('Binary OTA response timeout')
                if not reply.get('ok') or reply.get('offset')!=offset+len(chunk):raise RuntimeError('Binary OTA write rejected: '+str(reply.get('code')))
                progress(offset+len(chunk));break
            except (OSError,TimeoutError):
                if attempt==2:raise
                retries+=1
                # Allow a partially received binary block to leave its bounded raw
                # receive state before retrying the same idempotent offset/CRC.
                time.sleep(3.1);link.close();link.open()
    return {'binary_chunk_size':chunk_size,'retried_chunks':retries}

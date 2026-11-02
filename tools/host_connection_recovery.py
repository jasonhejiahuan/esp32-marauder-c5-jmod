"""Bounded Host_Connection quarantine recovery for the standalone OTA tool.

Only a valid, explicit RECOVERY_NOTICE permits a framed CLOSE. No speculative
binary upgrade, radio operation, or conversion of binary content to text logs.
"""
import re
import struct
import time
import zlib

HEADER=struct.Struct('<BBBBHHIII')
MAX_ENCODED=3109

def decode(encoded):
    raw=bytearray();pos=0
    while pos<len(encoded):
        code=encoded[pos];pos+=1
        if not code or pos+code-1>len(encoded):raise ValueError('COBS')
        raw.extend(encoded[pos:pos+code-1]);pos+=code-1
        if code!=255 and pos<len(encoded):raw.append(0)
    if len(raw)<24:raise ValueError('short frame')
    version,channel,kind,flags,op,size,session,request,sequence=HEADER.unpack_from(raw)
    if version!=1 or channel>5 or kind not in range(1,6) or flags or size>3072 or len(raw)!=24+size:
        raise ValueError('header')
    if zlib.crc32(raw[:-4])!=struct.unpack_from('<I',raw,len(raw)-4)[0]:raise ValueError('CRC')
    return channel,kind,op,session,request,sequence,bytes(raw[20:-4])

def close_frame(session,sequence):
    raw=HEADER.pack(1,0,1,0,3,0,session,1,sequence)
    raw+=struct.pack('<I',zlib.crc32(raw));out=bytearray(b'\0');at=0;code=1
    for b in raw:
        if b==0:out[at]=code;at=len(out);out.append(0);code=1
        else:
            out.append(b);code+=1
            if code==255:out[at]=code;at=len(out);out.append(0);code=1
    out[at]=code;return bytes(out)+b'\0'

class RecoveryDecoder:
    def __init__(self):self.buffer=bytearray();self.discard=False
    def receive(self,data):
        for byte in data:
            if byte==0:
                if self.buffer and not self.discard:
                    try:frame=decode(self.buffer)
                    except ValueError:pass
                    else:yield frame
                self.buffer.clear();self.discard=False
            elif not self.discard:
                if len(self.buffer)>=MAX_ENCODED:self.buffer.clear();self.discard=True
                else:self.buffer.append(byte)

def notice(frame):
    channel,kind,op,session,request,sequence,payload=frame
    if (channel,kind,op,request)!=(0,3,8,0) or not session:return None
    if len(payload)!=58 or payload[:2]!=b'\x11\0' or payload[19:21]!=b'\x20\0':return None
    device=payload[2:19];nonce=payload[21:53];rx,endpoint=struct.unpack_from('<IB',payload,53)
    if not re.fullmatch(rb'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}',device) or not re.fullmatch(rb'[0-9a-fA-F]{32}',nonce) or endpoint!=0 or rx==0xffffffff:return None
    return session,rx,nonce

def recover(serial,advertisement):
    session,rx,nonce=advertisement;deadline=time.monotonic()+3;decoder=RecoveryDecoder()
    def write(data):
        offset=0
        while offset<len(data):
            if time.monotonic()>deadline:raise TimeoutError('Host_Connection close write timeout')
            n=serial.write(data[offset:])
            if n is None or not 0<=n<=len(data)-offset:raise OSError('invalid write count')
            offset+=n
            if not n:time.sleep(.001)
    write(b'\0'+close_frame(session,rx+1))
    confirmed=False
    while time.monotonic()<deadline:
        for frame in decoder.receive(serial.read(min(serial.in_waiting,4096) or 1)):
            ch,kind,op,s,req,seq,payload=frame
            if (ch,kind,op,s,req)==(0,2,3,session,1) and payload==b'\x20\0'+nonce:confirmed=True
        if confirmed:break
    if not confirmed:raise TimeoutError('Host_Connection close acknowledgment missing')
    write(b'host_connection cli '+nonce+b'\n');buf=b''
    while time.monotonic()<deadline:
        buf=(buf+serial.read(min(serial.in_waiting,4096) or 1))[-4096:]
        if b'@HOST_CONNECTION CLI\n' in buf:return
    raise TimeoutError('Host_Connection CLI boundary acknowledgment missing')

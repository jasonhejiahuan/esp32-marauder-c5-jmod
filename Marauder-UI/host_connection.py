"""Host_Connection v1 wire codec. No serial, JSON, or UI dependencies.

Little endian fields; CRC-32/ISO-HDLC (zlib), poly 0x04c11db7 reflected,
init/xorout 0xffffffff. CRC covers the unencoded header and payload.
"""
import dataclasses
import enum
import struct
import zlib

VERSION=1
MAX_PAYLOAD=3072
HEADER=struct.Struct('<BBBBHHIII')
MAX_RAW=HEADER.size+MAX_PAYLOAD+4
MAX_ENCODED=MAX_RAW+MAX_RAW//254+1
class Channel(enum.IntEnum): CONTROL=0; COMMAND=1; CAPTURE=2; FILE=3; TELEMETRY=4; CONSOLE=5
class Kind(enum.IntEnum): REQUEST=1; RESULT=2; EVENT=3; DATA=4; ERROR=5
class Op(enum.IntEnum):
    HELLO=1; PING=2; CLOSE=3; BIND_TOKEN=4; BIND=5; ROUTE=6; STATUS=7; RECOVERY_NOTICE=8; VERSION=9; LINK_DIAGNOSTICS=10; BAUD_PREPARE=11; BAUD_PROBE=12; BAUD_COMMIT=13
    CLI_EXEC=0x100; SELECTION_LIST=0x110; SELECTION_SET=0x111; SELECTION_CHANGED=0x112; FILE_OP=0x200; FILE_START=0x210; FILE_CREDIT=0x211
    FILE_CANCEL=0x212; FILE_RECORD=0x213; FILE_END=0x214
    TELEMETRY=0x300; CAPTURE_START=0x400; CAPTURE_STOP=0x401; CAPTURE_RECORD=0x402; CAPTURE_STATS=0x403; SNIFF_START=0x404
    CONSOLE=0x500
def cobs_encode(raw):
    out=bytearray(b'\0'); code_pos=0; code=1
    for b in raw:
        if b==0:
            out[code_pos]=code; code_pos=len(out); out.append(0); code=1
        else:
            out.append(b); code+=1
            if code==255:
                out[code_pos]=code; code_pos=len(out); out.append(0); code=1
    out[code_pos]=code
    return bytes(out)
def cobs_decode(encoded):
    out=bytearray(); i=0
    while i<len(encoded):
        code=encoded[i]; i+=1
        if not code or i+code-1>len(encoded): raise ValueError('COBS')
        out.extend(encoded[i:i+code-1]); i+=code-1
        if code!=255 and i<len(encoded): out.append(0)
    return bytes(out)
@dataclasses.dataclass(frozen=True)
class Frame:
    channel:int; kind:int; op:int; session:int; request:int=0; sequence:int=0; payload:bytes=b''; flags:int=0
    def encode(self):
        if len(self.payload)>MAX_PAYLOAD: raise ValueError('payload limit')
        raw=HEADER.pack(VERSION,self.channel,self.kind,self.flags,self.op,len(self.payload),self.session,self.request,self.sequence)+self.payload
        return cobs_encode(raw+struct.pack('<I',zlib.crc32(raw)))+b'\0'
    @staticmethod
    def decode(encoded):
        raw=cobs_decode(encoded)
        if len(raw)<24: raise ValueError('short frame')
        version,ch,kind,flags,op,size,session,req,seq=HEADER.unpack_from(raw)
        if version!=VERSION or ch>5 or kind not in range(1,6) or flags or size>MAX_PAYLOAD or len(raw)!=24+size:
            raise ValueError('header')
        if zlib.crc32(raw[:-4])!=struct.unpack_from('<I',raw,len(raw)-4)[0]: raise ValueError('CRC')
        return Frame(ch,kind,op,session,req,seq,raw[20:-4],flags)
class Decoder:
    def __init__(self): self.buffer=bytearray(); self.discard=False; self.errors=0; self.oversized=0
    def feed(self,data):
        for b in data:
            if b==0:
                if self.buffer and not self.discard:
                    try: frame=Frame.decode(self.buffer)
                    except ValueError: self.errors+=1
                    else: yield frame
                self.buffer.clear(); self.discard=False
            elif not self.discard:
                if len(self.buffer)>=MAX_ENCODED:
                    self.buffer.clear(); self.discard=True; self.oversized+=1
                else: self.buffer.append(b)
def string(value):
    data=value.encode('utf-8')
    if len(data)>2048 or '\0' in value: raise ValueError('string length/NUL')
    return struct.pack('<H',len(data))+data
class Reader:
    def __init__(self,data): self.data=memoryview(data); self.offset=0
    def take(self,n):
        if n<0 or self.offset+n>len(self.data): raise ValueError('truncated payload')
        value=self.data[self.offset:self.offset+n]; self.offset+=n; return bytes(value)
    def unpack(self,fmt): return struct.unpack('<'+fmt,self.take(struct.calcsize('<'+fmt)))
    def string(self,limit=2048):
        n,=self.unpack('H')
        if n>limit: raise ValueError('string limit')
        s=self.take(n).decode('utf-8','strict')
        if '\0' in s: raise ValueError('NUL')
        return s
    def end(self):
        if self.offset!=len(self.data): raise ValueError('trailing payload')

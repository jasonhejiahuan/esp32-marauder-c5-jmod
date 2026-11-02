#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// Host_Connection v1. Explicit LE encoding, CRC-32/ISO-HDLC, no native structs on wire.
namespace host_connection {
constexpr size_t MAX_PAYLOAD=3072, MAX_RAW=20+MAX_PAYLOAD+4, MAX_ENCODED=MAX_RAW+MAX_RAW/254+1;
enum Channel:uint8_t { CONTROL,COMMAND,CAPTURE,FILE_DATA,TELEMETRY,CONSOLE };
enum Kind:uint8_t { REQUEST=1,RESULT,EVENT,DATA,ERROR };
enum Op:uint16_t { HELLO=1,PING,CLOSE,BIND_TOKEN,BIND,ROUTE,STATUS,RECOVERY_NOTICE,GET_VERSION,LINK_DIAGNOSTICS,BAUD_PREPARE,BAUD_PROBE,BAUD_COMMIT,CLI_EXEC=0x100,SELECTION_LIST=0x110,SELECTION_SET,SELECTION_CHANGED,FILE_OP=0x200,
 FILE_START=0x210,FILE_CREDIT,FILE_CANCEL,FILE_RECORD,FILE_END,GET_TELEMETRY=0x300,
 CAPTURE_START=0x400,CAPTURE_STOP,CAPTURE_RECORD,CAPTURE_STATS,SNIFF_START,CONSOLE_TEXT=0x500 };
inline uint16_t get16(const uint8_t* p){return p[0]|uint16_t(p[1])<<8;}
inline uint32_t get32(const uint8_t* p){return get16(p)|uint32_t(get16(p+2))<<16;}
inline void put16(uint8_t* p,uint16_t v){p[0]=v;p[1]=v>>8;}
inline void put32(uint8_t* p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
inline uint32_t crc32(const uint8_t* p,size_t n,uint32_t c=0xffffffff){while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320:0);}return c^0xffffffff;}
struct Frame {uint8_t channel=0,kind=0,flags=0;uint16_t op=0,length=0;uint32_t session=0,request=0,sequence=0;uint8_t payload[MAX_PAYLOAD];};
// Requests may overtake unsent ordinary traffic for cancellation/control. Keep a
// bounded replay window; IDs outside it are indeterminate, never re-executed.
struct RequestWindow {
 uint32_t high=0;uint64_t seen=0;
 bool accept(uint32_t id) {
  if(!id)return false;
  if(id>high){uint32_t n=id-high;seen=n>=64?0:seen<<n;high=id;}
  uint32_t n=high-id;if(n>=64||(seen&(uint64_t(1)<<n)))return false;
  seen|=uint64_t(1)<<n;return true;
 }
};
inline size_t encode(const Frame& f,uint8_t* raw,uint8_t* out){
 if(f.length>MAX_PAYLOAD)return 0;
 raw[0]=1;raw[1]=f.channel;raw[2]=f.kind;raw[3]=f.flags;put16(raw+4,f.op);put16(raw+6,f.length);
 put32(raw+8,f.session);put32(raw+12,f.request);put32(raw+16,f.sequence);memcpy(raw+20,f.payload,f.length);put32(raw+20+f.length,crc32(raw,20+f.length));
 size_t pos=1,mark=0;uint8_t code=1;
 for(size_t i=0;i<24+f.length;i++){if(!raw[i]){out[mark]=code;mark=pos++;code=1;}else{out[pos++]=raw[i];if(++code==255){out[mark]=code;mark=pos++;code=1;}}}
 out[mark]=code;out[pos++]=0;return pos;
}
struct Decoder {
 uint8_t encoded[MAX_ENCODED],raw[MAX_RAW];size_t used=0;bool discard=false;uint32_t errors=0,oversized=0;
 void clear(){used=0;discard=false;}
 bool byte(uint8_t b,Frame& f){
  if(b){if(!discard){if(used==MAX_ENCODED){discard=true;used=0;++oversized;}else encoded[used++]=b;}return false;}
  if(discard||!used){clear();return false;}
  size_t i=0,n=0;bool ok=true;
  while(i<used){unsigned code=encoded[i++];if(!code||i+code-1>used||n+code-1>MAX_RAW){ok=false;break;}
   for(unsigned j=1;j<code;j++)raw[n++]=encoded[i++];
   if(code!=255&&i<used){if(n==MAX_RAW){ok=false;break;}raw[n++]=0;}
  }clear();
  if(!ok||n<24||raw[0]!=1||raw[1]>5||raw[2]<1||raw[2]>5||raw[3]||get16(raw+6)>MAX_PAYLOAD||n!=24+get16(raw+6)||get32(raw+n-4)!=crc32(raw,n-4)){++errors;return false;}
  f.channel=raw[1];f.kind=raw[2];f.flags=raw[3];f.op=get16(raw+4);f.length=get16(raw+6);f.session=get32(raw+8);f.request=get32(raw+12);f.sequence=get32(raw+16);memcpy(f.payload,raw+20,f.length);return true;
 }
};
struct Writer {uint8_t* p;size_t n=0,cap;bool ok=true;
 Writer(uint8_t* p,size_t cap):p(p),cap(cap){}
 void bytes(const void* q,size_t len){if(n+len>cap){ok=false;return;}memcpy(p+n,q,len);n+=len;}
 void u8(uint8_t v){bytes(&v,1);}void u16(uint16_t v){uint8_t b[2];put16(b,v);bytes(b,2);}void u32(uint32_t v){uint8_t b[4];put32(b,v);bytes(b,4);}
 void u64(uint64_t v){u32(v);u32(v>>32);}void str(const char* s){size_t len=strlen(s);u16(len);bytes(s,len);}
};
struct Reader {const uint8_t* p;size_t n,pos=0;bool ok=true;
 Reader(const uint8_t* p,size_t n):p(p),n(n){}
 const uint8_t* take(size_t len){if(pos+len>n){ok=false;return nullptr;}const uint8_t* q=p+pos;pos+=len;return q;}
 uint8_t u8(){auto q=take(1);return q?*q:0;}uint16_t u16(){auto q=take(2);return q?get16(q):0;}uint32_t u32(){auto q=take(4);return q?get32(q):0;}
 bool end(){return ok&&pos==n;}
};
}

#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
// Pure bounded frame decoding, used only by AP + STA + Beacon.
namespace discovery {
struct Observation {
  uint8_t mac[6]{}, ap[6]{}, ssid[32]{}, ssidLen=0, security=0, primary=0;
  uint16_t caps=0; char kind=0;
};
inline bool unicast(const uint8_t* p) {
  static const uint8_t zero[6]={}; return !(p[0]&1) && memcmp(p,zero,6);
}
inline uint16_t le16(const uint8_t* p){return p[0]|(uint16_t(p[1])<<8);}
inline bool decode(const uint8_t* p,size_t n,Observation& o) {
  if(n<24 || (p[0]&3)) return false;
  if(p[0]==0x80) {
    if(n<36 || !unicast(p+16)) return false;
    o.kind='a';memcpy(o.mac,p+16,6);o.caps=le16(p+34);
    bool wpa=false,rsn=false,sae=false,psk=false,enterprise=false,wapi=false,ssid=false;
    for(size_t i=36;i<n;) {
      if(n-i<2 || size_t(p[i+1])>n-i-2) return false;
      const uint8_t id=p[i],len=p[i+1];const uint8_t* v=p+i+2;
      if(id==0) {if(len>32)return false; o.ssidLen=len;memcpy(o.ssid,v,len);ssid=true;}
      if(id==3 && len==1 && v[0]>=1 && v[0]<=14)o.primary=v[0];
      if(id==61 && len>=1 && v[0]>=1 && v[0]<=196)o.primary=v[0];
      if(id==221 && len>=4 && !memcmp(v,"\x00\x50\xf2\x01",4))wpa=true;
      if(id==68)wapi=true;
      if(id==48) {
        if(len<8 || le16(v)!=1)return false;
        rsn=true;size_t j=6;uint16_t c=le16(v+j);j+=2;
        if(c>(len-j)/4)return false;j+=4*c;
        if(len-j<2)return false;c=le16(v+j);j+=2;
        if(c>(len-j)/4)return false;
        for(unsigned k=0;k<c;k++,j+=4) {
          if(memcmp(v+j,"\x00\x0f\xac",3))continue;
          const uint8_t akm=v[j+3];
          sae|=akm==8||akm==9;psk|=akm==2||akm==4||akm==6;
          enterprise|=akm==1||akm==3||akm==5||akm==11||akm==12||akm==13;
        }
      }
      i+=2+len;
    }
    if(!ssid)return false;
    o.security=wapi?8:sae?(psk?10:4):rsn?(enterprise?6:wpa?5:3):wpa?2:(o.caps&16)?1:0;
    return true;
  }
  // Infrastructure data only; WDS/IBSS and multicast receivers are not clients.
  if((p[0]&0x0c)!=8 || (p[1]&3)==0 || (p[1]&3)==3)return false;
  const bool toDS=(p[1]&3)==1;
  const uint8_t* ap=toDS?p+4:p+10;const uint8_t* sta=toDS?p+10:p+4;
  if(!unicast(ap)||!unicast(sta)||!memcmp(ap,sta,6))return false;
  o.kind='s';memcpy(o.mac,sta,6);memcpy(o.ap,ap,6);return true;
}
}

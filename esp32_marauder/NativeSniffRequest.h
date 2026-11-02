#pragma once
#include <stdint.h>
#include <stddef.h>
namespace host_connection {
// SNIFF_START: explicit bytes, never a reconstructed CLI string.
enum class SniffMode:uint8_t { Raw=1,Beacon,Probe,Deauth,Pmkid,Sae,Pwn,PineScan,MultiSsid,All,Combined };
struct NativeSniffRequest {
 SniffMode mode;uint8_t flags,channel;
 bool decode(const uint8_t* p,size_t n){
  if(n!=3||p[0]<1||p[0]>11||p[1]&~7)return false;
  mode=SniffMode(p[0]);flags=p[1];channel=p[2];
  if(mode!=SniffMode::Pmkid&&(flags||channel))return false;
  return (flags&1)?channel>0&&channel<=196:channel==0;
 }
 // Only raw and discovery modes bypass native PCAP-selection processing.
 bool nativeFilter()const{return mode!=SniffMode::Raw&&mode!=SniffMode::All&&mode!=SniffMode::Combined;}
};
}

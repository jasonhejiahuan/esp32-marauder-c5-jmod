"""Execute the exact firmware selection service with bounded host adapters."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
 p=Path(td);(p/'SelectionService.cpp').write_bytes((ROOT/'esp32_marauder/SelectionService.cpp').read_bytes())
 for name in ['SelectionService.h','HostConnectionCodec.h','DiscoveryFrame.h']:(p/name).write_bytes((ROOT/'esp32_marauder'/name).read_bytes())
 (p/'HostConnection.h').write_text('''#pragma once
#define HOST_CONNECTION_ENABLED 1
#include <algorithm>
#include <string>
#include <vector>
#include <cstdlib>
inline void* ps_calloc(size_t n,size_t size){return calloc(n,size);}
#include "HostConnectionCodec.h"
extern bool busy;
namespace host_connection {inline bool serviceBusy(){return busy;}}
''')
 (p/'CommandLine.h').write_text('''#pragma once
#include "HostConnection.h"
using String=std::string;
inline uint32_t millis(){return 1234;}
template<class T>struct LinkedList{std::vector<T> values;int size(){return values.size();}T get(int i){return values.at(i);}void set(int i,T v){values.at(i)=v;}bool add(T v){values.push_back(v);return true;}void pop(){values.pop_back();}void remove(int i){values.erase(values.begin()+i);}};
struct AccessPoint{String essid;uint8_t channel,bssid[6];bool selected;char beacon[2];int8_t rssi;LinkedList<uint16_t>* stations;uint16_t packets;uint8_t sec;bool wps;String man;bool has_msg_1,has_msg_2,has_msg_3,has_msg_4;uint32_t last_seen_ms;};
struct Station{uint8_t mac[6];bool selected;uint16_t packets,ap;uint32_t last_seen_ms;};
struct Wifi{bool scanning(){return busy;}};extern Wifi wifi_scan_obj;
extern LinkedList<AccessPoint>* access_points;extern LinkedList<Station>* stations;
''')
 for name,namespace,method in [('StorageService','storage_service','leased'),('DeviceObservability','device_observation','bleActive'),('UsbOta','usb_ota','active')]:
  (p/(name+'.h')).write_text(f'#pragma once\nnamespace {namespace} {{inline bool {method}(){{return false;}}}}')
 (p/'MemoryGuard.h').write_text('''#pragma once
extern bool allocationsAllowed;
namespace marauder{struct RuntimeMemoryGuard{static RuntimeMemoryGuard& instance(){static RuntimeMemoryGuard s;return s;}bool allow(size_t){return allocationsAllowed;}};template<class L,class V>bool memoryGuardedAdd(L* l,V v){return allocationsAllowed&&l->add(v);}}
''')
 (p/'main.cpp').write_text('''#include "CommandLine.h"
#include "SelectionService.h"
#include <cassert>
#include <iostream>
bool busy=false,allocationsAllowed=true;Wifi wifi_scan_obj;LinkedList<AccessPoint> aps;LinkedList<Station> stas;auto* access_points=&aps;auto* stations=&stas;
uint8_t input[256],output[3072];
const char* set(uint8_t kind,uint8_t chosen,uint8_t last){host_connection::Writer w(input,sizeof(input));w.u8(kind);w.u8(chosen);uint8_t mac[]={2,1,2,3,4,last};w.bytes(mac,6);w.u8(6);w.u8(-65);w.u8(255);w.u8(3);w.bytes("lab",3);mac[5]=1;w.bytes(mac,6);host_connection::Reader r(input,w.n);host_connection::Writer result(output,sizeof(output));return selection_service::set(r,result);}
int main(){
 selection_service::begin();
 assert(!set(0,1,1));assert(aps.size()==1&&aps.get(0).selected&&aps.get(0).sec==255);
 assert(!set(0,1,1)&&aps.size()==1);assert(!set(0,0,1)&&!aps.get(0).selected);
 busy=true;assert(std::string(set(0,1,2))=="selection_requires_idle");assert(aps.size()==1);busy=false;
 assert(!set(1,1,2));assert(stas.size()==1&&stas.get(0).selected&&aps.get(0).stations->size()==1);
 assert(std::string(set(2,1,3))=="selection_request");assert(std::string(set(0,2,3))=="selection_request");
 host_connection::Writer w(input,sizeof(input));w.u8(0);w.u16(0);w.u32(0);host_connection::Reader r(input,w.n);host_connection::Writer out(output,sizeof(output));assert(!selection_service::list(r,out));
 host_connection::Reader rows(output,out.n);assert(rows.u8()==0);uint32_t fingerprint=rows.u32();assert(rows.u16()==1&&rows.u16()==0&&rows.u16()==1&&rows.u16()==0);assert(rows.take(6)[5]==1&&rows.u8()==0&&rows.end());
 assert(!set(0,1,1));host_connection::Writer newer(input,sizeof(input));newer.u8(0);newer.u16(0);newer.u32(fingerprint);host_connection::Reader stale(input,newer.n);host_connection::Writer sink(output,sizeof(output));assert(std::string(selection_service::list(stale,sink))=="selection_changed");
 // Start with empty firmware lists: discovery must register without selection.
 for(auto& ap:aps.values)delete ap.stations;aps.values.clear();stas.values.clear();
 discovery::Observation a{};a.kind='a';uint8_t address[]={2,1,2,3,4,10};memcpy(a.mac,address,6);memcpy(a.ssid,"first",5);a.ssidLen=5;a.security=3;a.caps=0x431;
 busy=true;assert(!selection_service::observe(a,149,-71,100)); // scanning is expected here
 assert(aps.size()==1&&!aps.get(0).selected&&aps.get(0).essid=="first"&&aps.get(0).channel==149);
 host_connection::Writer lw(input,sizeof(input));lw.u8(0);lw.u16(0);lw.u32(0);host_connection::Reader lr(input,lw.n);host_connection::Writer lo(output,sizeof(output));assert(!selection_service::list(lr,lo));
 host_connection::Reader discovered(output,lo.n);assert(discovered.u8()==0);discovered.u32();assert(discovered.u16()==1&&discovered.u16()==0&&discovered.u16()==1&&discovered.u16()==0);assert(discovered.take(6)[5]==10&&discovered.u8()==0&&discovered.end());
 busy=false;assert(!set(0,1,10));assert(aps.size()==1&&aps.get(0).selected);
 a.ssidLen=0;assert(!selection_service::observe(a,6,-50,200));assert(aps.size()==1&&aps.get(0).selected&&aps.get(0).essid=="first"&&aps.get(0).channel==6);
 assert(!set(0,0,10));assert(aps.size()==1&&!aps.get(0).selected); // deselect is not delete
 for(unsigned i=0;i<1000;i++)assert(!selection_service::observe(a,6,-50,201+i));assert(aps.size()==1);
 discovery::Observation sta{};sta.kind='s';memcpy(sta.ap,address,6);memcpy(sta.mac,address,6);sta.mac[5]=11;
 assert(!selection_service::observe(sta,6,-40,2000));assert(stas.size()==1&&!stas.get(0).selected&&aps.get(0).stations->size()==1);
 auto station=stas.get(0);station.selected=true;stas.set(0,station);
 assert(!selection_service::observe(sta,6,-40,2001));assert(stas.size()==1&&stas.get(0).selected&&aps.get(0).stations->size()==1);
 sta.ap[5]=12;assert(!selection_service::observe(sta,11,-60,2002));assert(aps.size()==2&&stas.get(0).ap==1&&stas.get(0).selected&&aps.get(0).stations->size()==0&&aps.get(1).stations->size()==1);
 a.mac[5]=12;a.ssidLen=5;assert(!selection_service::observe(a,11,-65,2003));assert(aps.size()==2&&aps.get(1).essid=="first"&&aps.get(1).stations->size()==1);
 allocationsAllowed=false;a.mac[5]=13;assert(std::string(selection_service::observe(a,11,-65,2004))=="selection_memory");assert(aps.size()==2);
 sta.ap[5]=10;assert(std::string(selection_service::observe(sta,6,-40,2005))=="selection_memory");assert(stas.get(0).ap==1&&aps.get(1).stations->size()==1&&aps.get(0).stations->size()==0);allocationsAllowed=true;
 a.mac[0]=1;assert(std::string(selection_service::observe(a,11,-65,2006))=="discovery_metadata");assert(aps.size()==2);
 sta.mac[0]=1;assert(std::string(selection_service::observe(sta,11,-65,2006))=="discovery_metadata");assert(stas.size()==1);
 // Deferred weak observations survive a temporary memory refusal, without radio waits.
 busy=true;selection_service::settle();assert(aps.size()==2);busy=false;
 selection_service::settle();assert(aps.size()==3&&aps.get(2).essid=="first"&&!aps.get(2).selected);
 assert(stas.get(0).ap==0&&stas.get(0).selected&&aps.get(0).stations->size()==1&&aps.get(1).stations->size()==0);
 selection_service::settle();assert(aps.size()==3);
 allocationsAllowed=false;a.mac[0]=2;a.mac[5]=14;assert(std::string(selection_service::observe(a,1,-80,2100))=="selection_memory");selection_service::discardDeferred();allocationsAllowed=true;selection_service::settle();assert(aps.size()==3);
 // The bounded retry pool rejects newest excess records explicitly.
 allocationsAllowed=false;
 for(unsigned i=0;i<128;i++){a.mac[4]=1;a.mac[5]=i;assert(std::string(selection_service::observe(a,1,-80,2200+i))=="selection_memory");}
 a.mac[4]=2;assert(std::string(selection_service::observe(a,1,-80,2400))=="discovery_deferred_full");
 selection_service::discardDeferred();allocationsAllowed=true;
 for(auto& ap:aps.values)delete ap.stations;aps.values.clear();stas.values.clear();
 a.mac[0]=2;assert(!selection_service::observe(a,1,-70,3000));assert(aps.size()==1&&!aps.get(0).selected); // clear/re-discovery
 for(auto& ap:aps.values)delete ap.stations;
 std::cout<<"PASS: actual firmware selection service; identity, idempotent state, parent association, bounds, busy guard, snapshot change; discovery before selection, hidden-name retention, duplicates, roaming, memory failures, clear/re-discovery"<<std::endl;
}
''')
 subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-g','-I',str(p),str(p/'SelectionService.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

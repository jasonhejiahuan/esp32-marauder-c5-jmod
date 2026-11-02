#include "HostConnection.h"
#ifdef HOST_CONNECTION_ENABLED
#include "SelectionService.h"
#include "CommandLine.h"
#include "StorageService.h"
#include "DeviceObservability.h"
#include "UsbOta.h"
#include "MemoryGuard.h"
#include <new>
#include <stdio.h>
namespace selection_service {
// Called only by the serialized service executor; identity is MAC, never a stale UI index.
constexpr unsigned LIMIT=4096,PAGE=128;
// The radio can temporarily exhaust the guarded internal heap. Retain only
// failed observations in PSRAM and retry after radio shutdown; never bypass the guard.
struct Deferred {discovery::Observation item;uint32_t at;uint8_t channel;int8_t rssi;bool used;};
constexpr unsigned DEFERRED_LIMIT=128;
Deferred* deferred=nullptr;
void begin(){if(!deferred)deferred=static_cast<Deferred*>(ps_calloc(DEFERRED_LIMIT,sizeof(Deferred)));}
void discardDeferred(){if(deferred)memset(deferred,0,DEFERRED_LIMIT*sizeof(Deferred));}
bool validMac(const uint8_t* m){if(!m||(m[0]&1))return false;uint8_t any=0;for(unsigned i=0;i<6;i++)any|=m[i];return any;}
int find(uint8_t kind,const uint8_t* mac){
 int n=kind?stations->size():access_points->size();
 for(int i=0;i<n;i++){if(kind){auto item=stations->get(i);if(!memcmp(item.mac,mac,6))return i;}
 else{auto item=access_points->get(i);if(!memcmp(item.bssid,mac,6))return i;}}return -1;
}
const char* list(host_connection::Reader& r,host_connection::Writer& w){
 uint8_t kind=r.u8();uint16_t offset=r.u16();uint32_t expected=r.u32();
 if(!r.end()||kind>1||!access_points||!stations)return "selection_request";
 unsigned total=kind?stations->size():access_points->size();if(total>LIMIT||offset>total)return "selection_limit";
 // Snapshot fingerprint catches reordered indices, source changes, or CLI changes between pages.
 uint32_t identity=2166136261u;
 for(unsigned i=0;i<total;i++){uint8_t mac[6];bool selected;if(kind){auto x=stations->get(i);memcpy(mac,x.mac,6);selected=x.selected;}else{auto x=access_points->get(i);memcpy(mac,x.bssid,6);selected=x.selected;}
 for(auto b:mac)identity=(identity^b)*16777619u;identity=(identity^selected)*16777619u;}
 if(!identity)identity=1;if(expected&&identity!=expected)return "selection_changed";
 unsigned count=std::min<unsigned>(PAGE,total-offset);w.u8(kind);w.u32(identity);w.u16(total);w.u16(offset);w.u16(count);
 for(unsigned i=offset;i<offset+count;i++){w.u16(i);if(kind){auto x=stations->get(i);w.bytes(x.mac,6);w.u8(x.selected);}else{auto x=access_points->get(i);w.bytes(x.bssid,6);w.u8(x.selected);}}
 return nullptr;
}
const char* addAP(const uint8_t* mac,uint8_t channel,int8_t rssi,uint8_t security,const uint8_t* name,uint8_t nameLen){
 if(access_points->size()>=LIMIT)return "selection_limit";
 if(!marauder::RuntimeMemoryGuard::instance().allow(sizeof(AccessPoint)+sizeof(LinkedList<uint16_t>)+128))return "selection_memory";
 AccessPoint item{};memcpy(item.bssid,mac,6);
 if(nameLen)item.essid=String(reinterpret_cast<const char*>(name),nameLen);
 else{char address[18];snprintf(address,sizeof(address),"%02x:%02x:%02x:%02x:%02x:%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);item.essid=address;}
 item.channel=channel;item.rssi=rssi;item.sec=security;item.last_seen_ms=millis();item.stations=new(std::nothrow) LinkedList<uint16_t>();
 if(!item.stations)return "selection_memory";
 if(!marauder::memoryGuardedAdd(access_points,item)){delete item.stations;return "selection_memory";}return nullptr;
}
const char* observeNow(const discovery::Observation& o,uint8_t channel,int8_t rssi,uint32_t at){
 if(!access_points||!stations||!validMac(o.mac)||!channel||channel>196||rssi>0||o.ssidLen>32)return "discovery_metadata";
 if(o.kind=='a'){
  int index=find(0,o.mac);
  if(index<0){const char* failure=addAP(o.mac,channel,rssi,o.security,o.ssid,o.ssidLen);if(failure)return failure;index=access_points->size()-1;}
  auto item=access_points->get(index);
  // Hidden/repeated beacons must not erase a learned name or selection state.
  if(o.ssidLen&&(item.essid.length()!=o.ssidLen||memcmp(item.essid.c_str(),o.ssid,o.ssidLen)))item.essid=String(reinterpret_cast<const char*>(o.ssid),o.ssidLen);
  item.channel=channel;item.rssi=rssi;item.sec=o.security;item.last_seen_ms=at;
  item.beacon[0]=o.caps&255;item.beacon[1]=o.caps>>8;
  access_points->set(index,item);return nullptr;
 }
 if(o.kind!='s'||!validMac(o.ap)||!memcmp(o.mac,o.ap,6))return "discovery_metadata";
 int index=find(1,o.mac),ap=find(0,o.ap);
 if(index<0&&stations->size()>=LIMIT)return "selection_limit";
 if(ap<0){const char* failure=addAP(o.ap,channel,-127,255,reinterpret_cast<const uint8_t*>(""),0);if(failure)return failure;ap=access_points->size()-1;}
 auto owner=access_points->get(ap);
 if(!owner.stations)return "discovery_parent";
 if(index<0){
  Station item{};memcpy(item.mac,o.mac,6);item.ap=ap;item.last_seen_ms=at;
  if(!marauder::memoryGuardedAdd(stations,item))return "selection_memory";
  index=stations->size()-1;
  if(!marauder::memoryGuardedAdd(owner.stations,uint16_t(index))){stations->pop();return "selection_memory";}
 }else{
  auto item=stations->get(index);
  if(item.ap!=ap){
   // Establish the new relation first; failed allocation leaves the old one valid.
   if(!marauder::memoryGuardedAdd(owner.stations,uint16_t(index)))return "selection_memory";
   if(item.ap<access_points->size()){
    auto previous=access_points->get(item.ap);
    if(previous.stations)for(int i=previous.stations->size()-1;i>=0;--i)if(previous.stations->get(i)==index)previous.stations->remove(i);
   }
   item.ap=ap;
  }
  item.last_seen_ms=at;stations->set(index,item);
 }
 return nullptr;
}
const char* observe(const discovery::Observation& o,uint8_t channel,int8_t rssi,uint32_t at){
 const char* failure=observeNow(o,channel,rssi,at);
 if(!deferred)return failure;
 Deferred* slot=nullptr;
 for(unsigned i=0;i<DEFERRED_LIMIT;i++){
  auto& d=deferred[i];
  if(d.used&&d.item.kind==o.kind&&!memcmp(d.item.mac,o.mac,6)){slot=&d;break;}
  if(!d.used&&!slot)slot=&d;
 }
 if(!failure){if(slot&&slot->used)slot->used=false;return nullptr;}
 if(strcmp(failure,"selection_memory"))return failure;
 if(!slot)return "discovery_deferred_full"; // drop newest; caller counts the failure
 // Keep a known SSID if the latest retry is a hidden beacon.
 auto retained=o;
 if(slot->used&&!o.ssidLen&&slot->item.ssidLen){retained.ssidLen=slot->item.ssidLen;memcpy(retained.ssid,slot->item.ssid,retained.ssidLen);}
 *slot={retained,at,channel,rssi,true};return failure;
}
void settle(){
 if(!deferred||wifi_scan_obj.scanning())return;
 for(unsigned i=0;i<DEFERRED_LIMIT;i++){
  auto& d=deferred[i];if(d.used&&!observeNow(d.item,d.channel,d.rssi,d.at))d.used=false;
 }
}
const char* set(host_connection::Reader& r,host_connection::Writer& w){
 uint8_t kind=r.u8(),selected=r.u8();const uint8_t* mac=r.take(6);uint8_t channel=r.u8();int8_t rssi=static_cast<int8_t>(r.u8());uint8_t security=r.u8();uint8_t nameLen=r.u8();const uint8_t* name=r.take(nameLen);const uint8_t* parent=r.take(6);
 if(!r.end()||kind>1||selected>1||nameLen>32||!validMac(mac)||!parent||!access_points||!stations)return "selection_request";
 if(wifi_scan_obj.scanning()||device_observation::bleActive()||host_connection::serviceBusy()||storage_service::leased()||usb_ota::active())return "selection_requires_idle";
 int index=find(kind,mac);
 if(index<0){
  if(!selected)return "selection_missing";
  if(!channel||channel>196||rssi>0||(security>10&&security!=255))return "selection_metadata";
  if((kind?stations->size():access_points->size())>=LIMIT)return "selection_limit";
  if(kind){
   if(!validMac(parent)||!memcmp(parent,mac,6))return "selection_parent_missing";
   int ap=find(0,parent);if(ap<0){const char* failure=addAP(parent,channel,-127,255,reinterpret_cast<const uint8_t*>(""),0);if(failure)return failure;ap=find(0,parent);}
   Station item{};memcpy(item.mac,mac,6);item.ap=ap;item.last_seen_ms=millis();
   if(!marauder::memoryGuardedAdd(stations,item))return "selection_memory";
   auto owner=access_points->get(ap);if(!marauder::memoryGuardedAdd(owner.stations,stations->size()-1)){stations->pop();return "selection_memory";}
  }else{
   const char* failure=addAP(mac,channel,rssi,security,name,nameLen);if(failure)return failure;
  }
  index=find(kind,mac);
 }
 if(kind){auto item=stations->get(index);item.selected=selected;stations->set(index,item);}else{auto item=access_points->get(index);item.selected=selected;access_points->set(index,item);}
 w.u8(kind);w.u16(index);w.bytes(mac,6);w.u8(selected);return nullptr;
}
}
#endif

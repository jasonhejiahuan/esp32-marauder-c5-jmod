#include "HostConnection.h"
#include "CombinedDiscovery.h"
#include "DiscoveryFrame.h"
#ifdef HOST_CONNECTION_ENABLED
#include "SelectionService.h"
#endif
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
#include "hal/usb_serial_jtag_ll.h"
#endif
namespace combined {
namespace {
constexpr size_t CAPACITY=1024, PROBES=32;
struct Entry {discovery::Observation item;uint32_t count,last,sent,registeredAt;int8_t rssi;uint8_t channel;bool used,dirty,registered;};
Entry* table=nullptr;
struct Packet {discovery::Observation item;uint32_t at;int8_t rssi;uint8_t channel;};
QueueHandle_t queue=nullptr;
std::atomic<uint32_t> queueDrops{0};
std::atomic<bool> running{false};
uint32_t lastPump=0,lastStatus=0,beacons=0,frames=0,overflow=0,registryErrors=0;
size_t cursor=0;
size_t hash(const discovery::Observation& o){uint32_t h=o.kind;for(auto b:o.mac)h=(h^b)*16777619u;return h&(CAPACITY-1);}
void mac(char* out,const uint8_t* p){snprintf(out,18,"%02x:%02x:%02x:%02x:%02x:%02x",p[0],p[1],p[2],p[3],p[4],p[5]);}
}
bool begin() {
  if(!table)table=static_cast<Entry*>(heap_caps_calloc(CAPACITY,sizeof(Entry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
  if(!queue)queue=xQueueCreate(128,sizeof(Packet));
  if(!table||!queue)return false;
  xQueueReset(queue);queueDrops.store(0);

  memset(table,0,CAPACITY*sizeof(Entry));cursor=0;lastPump=lastStatus=0;beacons=frames=overflow=registryErrors=0;running=true;return true;
}
void receive(void* buf,wifi_promiscuous_pkt_type_t type) {
  if(!running || (type!=WIFI_PKT_MGMT && type!=WIFI_PKT_DATA))return;
  const auto* p=static_cast<const wifi_promiscuous_pkt_t*>(buf);
  if(p->rx_ctrl.rx_state || p->rx_ctrl.sig_len<4)return;
  discovery::Observation o{};
  if(!discovery::decode(p->payload,p->rx_ctrl.sig_len-4,o))return;
  Packet packet{o,millis(),static_cast<int8_t>(p->rx_ctrl.rssi),static_cast<uint8_t>(o.primary?o.primary:p->rx_ctrl.channel)};
  if(xQueueSend(queue,&packet,0)!=pdTRUE)queueDrops.fetch_add(1,std::memory_order_relaxed);
}

void pump(uint32_t now) {
  if(!running || uint32_t(now-lastPump)<20)return;lastPump=now;
  Packet packet{};
  for(unsigned drain=0;drain<128 && xQueueReceive(queue,&packet,0)==pdTRUE;drain++) {
    const auto& o=packet.item;
    if(o.kind=='a')++beacons;else ++frames;
    size_t h=hash(o);Entry* entry=nullptr;
    for(size_t k=0;k<PROBES;k++) {
      Entry& e=table[(h+k)&(CAPACITY-1)];
      if(!e.used || (e.item.kind==o.kind&&!memcmp(e.item.mac,o.mac,6))){entry=&e;break;}
    }
    if(entry){
      #ifdef HOST_CONNECTION_ENABLED
      // This pump is on the service executor, never the Wi-Fi receive callback.
      // Register before publishing to the UI; repeated packets refresh at most once/s.
      bool metadataChanged=entry->channel!=packet.channel||entry->item.security!=o.security||entry->item.ssidLen!=o.ssidLen||
        memcmp(entry->item.ssid,o.ssid,o.ssidLen)||memcmp(entry->item.ap,o.ap,6);
      if(!entry->registered||metadataChanged||uint32_t(packet.at-entry->registeredAt)>=1000){
        if(selection_service::observe(o,packet.channel,packet.rssi,packet.at))++registryErrors;
        entry->registered=true;entry->registeredAt=packet.at;
      }
      #endif
      entry->item=o;++entry->count;entry->last=packet.at;entry->rssi=packet.rssi;entry->channel=packet.channel;entry->used=entry->dirty=true;
    }
    else ++overflow;
  }
  now=millis();
  #if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
    // availableForWrite alone cannot restart a stalled native USB IN transfer.
    // Re-arm the same IN-empty interrupt used by HWCDC::write, without waiting
    // or clearing queued bytes. RX callbacks and legacy mode timeouts are unchanged.
    if (host_connection::console().availableForWrite()<256) {
      usb_serial_jtag_ll_txfifo_flush();
      usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
    }
  #endif
  // No serial I/O or allocation in the Wi-Fi callback. Bound main-loop work.
  for(size_t checked=0,emitted=0;checked<CAPACITY && emitted<4;checked++) {
    if(host_connection::console().availableForWrite()<256)break;
    const size_t i=cursor;cursor=(cursor+1)&(CAPACITY-1);Entry copy{};bool ready=false;
    Entry& e=table[i];
    if(e.used&&e.dirty&&(!e.sent||uint32_t(now-e.sent)>=1000)){copy=e;e.dirty=false;e.sent=now;ready=true;}
    if(!ready)continue;
    char line[256],addr[18],ap[18],ssid[65];mac(addr,copy.item.mac);mac(ap,copy.item.ap);
    for(unsigned j=0;j<copy.item.ssidLen;j++)snprintf(ssid+j*2,3,"%02x",copy.item.ssid[j]);ssid[copy.item.ssidLen*2]=0;
    int n;
    if(copy.item.kind=='a')n=snprintf(line,sizeof(line),"@DISC:{\"k\":\"a\",\"mac\":\"%s\",\"ssid\":\"%s\",\"sec\":%u,\"cap\":%u,\"r\":%d,\"ch\":%u,\"n\":%lu,\"age\":%lu}\n",addr,ssid,copy.item.security,copy.item.caps,copy.rssi,copy.channel,(unsigned long)copy.count,(unsigned long)(now-copy.last));
    else n=snprintf(line,sizeof(line),"@DISC:{\"k\":\"s\",\"mac\":\"%s\",\"ap\":\"%s\",\"r\":%d,\"ch\":%u,\"n\":%lu,\"age\":%lu}\n",addr,ap,copy.rssi,copy.channel,(unsigned long)copy.count,(unsigned long)(now-copy.last));
    if(n>0 && n<int(sizeof(line)))host_connection::console().write(reinterpret_cast<const uint8_t*>(line),n);
    ++emitted;
  }
  if(uint32_t(now-lastStatus)>=1000 && host_connection::console().availableForWrite()>=160) {
    uint32_t b=beacons,f=frames,d=overflow+queueDrops.load(std::memory_order_relaxed);
    host_connection::console().printf("@DISC:{\"k\":\"status\",\"beacons\":%lu,\"data\":%lu,\"overflow\":%lu,\"registry_errors\":%lu}\n",(unsigned long)b,(unsigned long)f,(unsigned long)d,(unsigned long)registryErrors);lastStatus=now;
  }
}
void end(){
  running=false;

}
}

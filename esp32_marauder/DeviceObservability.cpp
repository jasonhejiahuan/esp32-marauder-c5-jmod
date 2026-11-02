#include "HostConnection.h"
#include "DeviceObservability.h"
#if defined(MARAUDER_C5)
#include "WiFiScan.h"
#include "StorageBackend.h"
#include <SPIFFS.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <driver/temperature_sensor.h>
#include <freertos/queue.h>
#include <atomic>
#if ARDUINO_USB_CDC_ON_BOOT
#include "hal/usb_serial_jtag_ll.h"
#endif
extern WiFiScan wifi_scan_obj;
namespace device_observation {
namespace {
temperature_sensor_handle_t sensor=nullptr;
uint32_t appBytes=0,physicalBytes=0,flashWindow=0;
uint32_t cachedStorageTotal=0,cachedStorageUsed=0,cachedSettingsTotal=0,cachedSettingsUsed=0;
uint32_t storageSampleAt=0;bool storageSampleValid=false;
void sampleStorage(){
 // LittleFS/SPIFFS usedBytes may traverse flash. Do not put those traversals in
 // a live-capture telemetry request: flash/cache stalls also delay USB control.
 if(storageSampleValid&&(host_connection::capturing()||wifi_scan_obj.scanning()))return;
 if(storageSampleValid&&millis()-storageSampleAt<15000)return;
 cachedStorageTotal=storageTotalBytes();cachedStorageUsed=storageUsedBytes();
 cachedSettingsTotal=SPIFFS.totalBytes();cachedSettingsUsed=SPIFFS.usedBytes();
 storageSampleAt=millis();storageSampleValid=true;
}
struct Advertisement { char mac[18],name[65],payload[193],services[112]; int rssi,tx,company,appearance; uint16_t length; uint8_t type; bool connectable,truncated; uint32_t at; };
struct Entry { Advertisement ad; uint32_t count,lastSent; bool used,dirty; };
Entry* entries=nullptr;
QueueHandle_t queue=nullptr;
std::atomic<bool> active{false};
std::atomic<uint32_t> dropped{0},reports{0};
uint32_t lastBatch=0,lastStatus=0,tableFull=0;
uint16_t cursor=0;
void kick() {
 #if ARDUINO_USB_CDC_ON_BOOT
 if (Serial && host_connection::console().availableForWrite()<256) {
  usb_serial_jtag_ll_txfifo_flush();
  usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
 }
 #endif
}
void hex(char* dst,const uint8_t* src,size_t n) { const char* h="0123456789abcdef";for(size_t i=0;i<n;i++){dst[2*i]=h[src[i]>>4];dst[2*i+1]=h[src[i]&15];}dst[2*n]=0; }
class Callbacks: public NimBLEScanCallbacks {
 void onResult(const NimBLEAdvertisedDevice* d) override {
  if(!active.load() || !queue)return;
  Advertisement a{};a.at=millis();a.rssi=d->getRSSI();a.tx=d->haveTXPower()?d->getTXPower():127;
  a.company=-1;a.appearance=d->haveAppearance()?d->getAppearance():-1;a.type=d->getAddressType();a.connectable=d->isConnectable();
  auto address=d->getAddress().toString();snprintf(a.mac,sizeof(a.mac),"%s",address.c_str());
  auto name=d->getName();hex(a.name,reinterpret_cast<const uint8_t*>(name.data()),std::min(name.size(),size_t(32)));
  const auto& bytes=d->getPayload();a.length=bytes.size();a.truncated=bytes.size()>96;hex(a.payload,bytes.data(),std::min(bytes.size(),size_t(96)));
  if(d->haveManufacturerData()){auto m=d->getManufacturerData();if(m.size()>=2)a.company=uint8_t(m[0])|(uint8_t(m[1])<<8);}
  for(uint8_t i=0;i<std::min(uint8_t(3),d->getServiceUUIDCount());i++) {auto id=d->getServiceUUID(i).toString();size_t used=strlen(a.services);snprintf(a.services+used,sizeof(a.services)-used,"%s%s",used?",":"",id.c_str());}
  reports.fetch_add(1);if(xQueueSend(queue,&a,0)!=pdTRUE)dropped.fetch_add(1);
 }
} callbacks;
void status() {unsigned count=0;if(entries)for(unsigned i=0;i<256;i++)count+=entries[i].used;
 host_connection::console().printf("@BLE:{\"kind\":\"status\",\"active\":%s,\"devices\":%u,\"reports\":%u,\"dropped\":%u,\"tableFull\":%u}\n",active.load()?"true":"false",count,reports.load(),dropped.load(),tableFull);kick();}
}
void setup(){appBytes=ESP.getSketchSize();esp_flash_get_physical_size(nullptr,&physicalBytes);esp_flash_get_size(nullptr,&flashWindow);sampleStorage();}
bool bleActive(){return active.load();}
void binaryReport(host_connection::Writer& w) {
 sampleStorage();
 if(!sensor){temperature_sensor_config_t cfg=TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10,80);if(temperature_sensor_install(&cfg,&sensor)==ESP_OK){if(temperature_sensor_enable(sensor)!=ESP_OK){temperature_sensor_uninstall(sensor);sensor=nullptr;}}}
 float temp=NAN;if(sensor&&temperature_sensor_get_celsius(sensor,&temp)!=ESP_OK)temp=NAN;
 esp_chip_info_t chip;esp_chip_info(&chip);const auto* app=esp_ota_get_running_partition();
 w.u64(esp_timer_get_time()/1000);w.u32(isfinite(temp)?int32_t(temp*1000):INT32_MIN);
 uint32_t values[]={getCpuFrequencyMhz(),chip.cores,chip.revision,uint32_t(esp_reset_reason()),ESP.getHeapSize(),ESP.getFreeHeap(),ESP.getMinFreeHeap(),ESP.getMaxAllocHeap(),ESP.getPsramSize(),ESP.getFreePsram(),physicalBytes,flashWindow,appBytes,app?app->size:0,cachedStorageTotal,cachedStorageUsed,cachedSettingsTotal,cachedSettingsUsed,uint32_t(wifi_scan_obj.currentScanMode),uint32_t(active.load())};
 for(uint32_t v:values)w.u32(v);
}
void report(const char* transaction) {
 if(!sensor){temperature_sensor_config_t cfg=TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10,80);if(temperature_sensor_install(&cfg,&sensor)==ESP_OK){if(temperature_sensor_enable(sensor)!=ESP_OK){temperature_sensor_uninstall(sensor);sensor=nullptr;}}}
 float temp=NAN;if(sensor&&temperature_sensor_get_celsius(sensor,&temp)!=ESP_OK)temp=NAN;
 char temperature[20]="null";if(isfinite(temp))snprintf(temperature,sizeof(temperature),"%.1f",temp);
 esp_chip_info_t chip;esp_chip_info(&chip);
 const esp_partition_t* app=esp_ota_get_running_partition();
 host_connection::console().printf("@DEVICE:{\"v\":1,\"tx\":\"%s\",\"uptime_ms\":%llu,\"temperature_c\":%s,\"cpu_mhz\":%u,\"cores\":%u,\"revision\":%u,\"reset_reason\":%u,\"heap_total\":%u,\"heap_free\":%u,\"heap_min\":%u,\"heap_largest\":%u,\"psram_total\":%u,\"psram_free\":%u,\"flash_physical\":%u,\"flash_window\":%u,\"app_size\":%u,\"app_slot\":%u,\"storage_total\":%u,\"storage_used\":%u,\"settings_total\":%u,\"settings_used\":%u,\"wifi_mode\":%u,\"ble_active\":%s,\"radios\":[\"wifi6-dual-band\",\"ble\",\"802.15.4\"]}\n",
 transaction,(unsigned long long)(esp_timer_get_time()/1000),temperature,getCpuFrequencyMhz(),chip.cores,chip.revision,esp_reset_reason(),ESP.getHeapSize(),ESP.getFreeHeap(),ESP.getMinFreeHeap(),ESP.getMaxAllocHeap(),ESP.getPsramSize(),ESP.getFreePsram(),physicalBytes,flashWindow,appBytes,app?app->size:0,(unsigned)storageTotalBytes(),(unsigned)storageUsedBytes(),(unsigned)SPIFFS.totalBytes(),(unsigned)SPIFFS.usedBytes(),wifi_scan_obj.currentScanMode,active.load()?"true":"false");kick();
}
void stop() {
 if(!active.exchange(false))return;
 NimBLEDevice::getScan()->stop();NimBLEDevice::getScan()->clearResults();NimBLEDevice::deinit();
 if(queue)xQueueReset(queue);
 status();
}
void bleCommand(const String& action) {
 if(action=="stop"){stop();host_connection::console().println("BLE observation stopped");return;}
 if(action=="status"){status();return;}
 if(action=="clear") {if(active.load()){host_connection::console().println("BLE error: stop observation first");return;}if(entries)memset(entries,0,sizeof(Entry)*256);reports=0;dropped=0;tableFull=0;host_connection::console().println("@BLE:{\"kind\":\"clear\"}");status();return;}
 if(action!="start"){host_connection::console().println("bleobserve start|stop|status|clear - passive BLE advertisements");return;}
 if(active.load()){status();return;}
 if(wifi_scan_obj.scanning()||wifi_scan_obj.wifi_connected){host_connection::console().println("BLE error: stop WLAN activity first");return;}
 if(!entries)entries=static_cast<Entry*>(ps_calloc(256,sizeof(Entry)));
 if(!queue)queue=xQueueCreate(24,sizeof(Advertisement));
 if(!entries||!queue){host_connection::console().println("BLE error: allocation failed");return;}
 wifi_scan_obj.shutdownWiFi();wifi_scan_obj.shutdownBLE();
 if(!NimBLEDevice::init("")){host_connection::console().println("BLE error: initialization failed");return;}
 auto scan=NimBLEDevice::getScan();
 scan->setScanCallbacks(&callbacks,true);scan->setActiveScan(false);scan->setInterval(80);scan->setWindow(40);scan->setMaxResults(0);scan->setDuplicateFilter(false);
 active=true;
 if(!scan->start(0,false,true)){active=false;NimBLEDevice::deinit();host_connection::console().println("BLE error: scan failed");return;}
 host_connection::console().println("BLE observation started; passive");status();
}
void tick() {
 if(!active.load())return;
 Advertisement a;for(unsigned n=0;n<24&&xQueueReceive(queue,&a,0)==pdTRUE;n++){
  unsigned hash=2166136261u;for(const char* c=a.mac;*c;c++)hash=(hash^uint8_t(*c))*16777619u;hash^=a.type;Entry* e=nullptr;
  for(unsigned i=0;i<16;i++){auto& slot=entries[(hash+i)%256];if(!slot.used||(slot.ad.type==a.type&&strcmp(slot.ad.mac,a.mac)==0)){e=&slot;break;}}
  if(!e){tableFull++;continue;}e->used=true;e->ad=a;e->count++;e->dirty=true;
 }
 uint32_t now=millis();if(now-lastBatch>=200){lastBatch=now;unsigned sent=0;
  for(unsigned n=0;n<256&&sent<3;n++){if(host_connection::console().availableForWrite()<900){kick();break;}auto& e=entries[cursor++%256];if(!e.used||!e.dirty||(e.lastSent&&now-e.lastSent<1000))continue;e.dirty=false;e.lastSent=now;sent++;
   host_connection::console().printf("@BLE:{\"kind\":\"device\",\"mac\":\"%s\",\"address_type\":%u,\"name\":\"%s\",\"rssi\":%d,\"tx_power\":%d,\"company\":%d,\"appearance\":%d,\"connectable\":%s,\"services\":\"%s\",\"payload\":\"%s\",\"payload_length\":%u,\"truncated\":%s,\"count\":%u,\"age_ms\":%u}\n",e.ad.mac,e.ad.type,e.ad.name,e.ad.rssi,e.ad.tx,e.ad.company,e.ad.appearance,e.ad.connectable?"true":"false",e.ad.services,e.ad.payload,e.ad.length,e.ad.truncated?"true":"false",e.count,now-e.ad.at);
  }kick();
 }
 if(now-lastStatus>=2000){lastStatus=now;status();}
}
}
#endif

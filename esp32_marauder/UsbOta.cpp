#include "HostConnection.h"
#include "UsbOta.h"
#if defined(MARAUDER_C5) && (defined(MARAUDER_WAVESHARE_C5_RGB) || defined(MARAUDER_XIAO_C5)) && defined(MARAUDER_INTERNAL_STORAGE)
#include "StorageBackend.h"
#include "DeviceObservability.h"
#include "WiFiScan.h"
#include <SPIFFS.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_flash.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include "hal/usb_serial_jtag_ll.h"
#if !ARDUINO_USB_CDC_ON_BOOT || !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "This C5 OTA profile requires native USB CDC and bootloader rollback"
#endif
extern WiFiScan wifi_scan_obj;

// Arduino otherwise confirms the image before application setup and USB tests.
extern "C" bool verifyRollbackLater() { return true; }
namespace usb_ota {
namespace {
constexpr const char* profile=MARAUDER_BUILD_PROFILE;
esp_ota_handle_t handle=0;
const esp_partition_t* target=nullptr;
String session,expected,appHash,tableHash,bootHash,configHash;
uint32_t expectedSize=0,received=0,lastOffset=0,lastCRC=0,lastLength=0,lastActivity=0,rebootAt=0,verifyDeadline=0,drainUntil=0,appSize=0;
bool pending=false,healthy=false,committed=false;
mbedtls_sha256_context digest;
String hex(const uint8_t* p,size_t n){const char* h="0123456789abcdef";String s;s.reserve(n*2);for(size_t i=0;i<n;i++){s+=h[p[i]>>4];s+=h[p[i]&15];}return s;}
bool token(const String& s){if(s.isEmpty()||s.length()>40)return false;for(char c:s)if(!isalnum(uint8_t(c))&&c!='_'&&c!='-')return false;return true;}
bool sha(const String& s){if(s.length()!=64)return false;for(char c:s)if(!isxdigit(uint8_t(c)))return false;return true;}
bool number(const String& s,uint32_t& n){if(s.isEmpty()||s.length()>10)return false;uint64_t v=0;for(char c:s){if(c<'0'||c>'9')return false;v=v*10+c-'0';if(v>UINT32_MAX)return false;}n=v;return true;}
uint32_t crc(const uint8_t* p,size_t n){uint32_t c=0xffffffff;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320:0);}return c^0xffffffff;}
void kick(){drainUntil=millis()+2000;usb_serial_jtag_ll_txfifo_flush();usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);}
void reply(const String& tx,bool ok,const char* code){host_connection::console().printf("\n@OTA:{\"tx\":\"%s\",\"ok\":%s,\"code\":\"%s\",\"offset\":%u}\n",tx.c_str(),ok?"true":"false",code,received);kick();}
String flashHash(uint32_t address,uint32_t length){
 mbedtls_sha256_context c;mbedtls_sha256_init(&c);mbedtls_sha256_starts(&c,0);uint8_t buf[1024],hash[32];bool ok=true;
 for(uint32_t o=0;o<length;o+=sizeof(buf)){size_t n=std::min<size_t>(sizeof(buf),length-o);if(esp_flash_read(nullptr,buf,address+o,n)!=ESP_OK){ok=false;break;}mbedtls_sha256_update(&c,buf,n);yield();}
 mbedtls_sha256_finish(&c,hash);mbedtls_sha256_free(&c);return ok?hex(hash,32):String();
}
String settingsHash(){File f=SPIFFS.open("/settings.json",FILE_READ);if(!f||!f.size())return "";mbedtls_sha256_context c;mbedtls_sha256_init(&c);mbedtls_sha256_starts(&c,0);uint8_t b[512],h[32];while(f.available()){size_t n=f.read(b,sizeof(b));if(!n){f.close();mbedtls_sha256_free(&c);return "";}mbedtls_sha256_update(&c,b,n);}f.close();mbedtls_sha256_finish(&c,h);mbedtls_sha256_free(&c);return hex(h,32);}
bool layout(){auto a=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_0,"app0");auto b=esp_partition_find_first(ESP_PARTITION_TYPE_APP,ESP_PARTITION_SUBTYPE_APP_OTA_1,"app1");auto s=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"spiffs");auto f=esp_partition_find_first(ESP_PARTITION_TYPE_DATA,ESP_PARTITION_SUBTYPE_DATA_SPIFFS,"storage");return a&&b&&s&&f&&a->address==0x10000&&b->address==MARAUDER_APP1_OFFSET&&a->size==MARAUDER_APP_SLOT_SIZE&&b->size==MARAUDER_APP_SLOT_SIZE&&s->address==MARAUDER_SETTINGS_OFFSET&&s->size==MARAUDER_SETTINGS_SIZE&&f->address==MARAUDER_STORAGE_OFFSET&&f->size==MARAUDER_STORAGE_SIZE;}
void abortTransfer(){if(handle){esp_ota_abort(handle);handle=0;mbedtls_sha256_free(&digest);}session="";target=nullptr;expectedSize=received=0;committed=false;}
void status(const String& tx){auto run=esp_ota_get_running_partition();auto next=esp_ota_get_next_update_partition(nullptr);esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;esp_ota_get_state_partition(run,&state);
 host_connection::console().printf("\n@OTA:{\"tx\":\"%s\",\"ok\":true,\"profile\":\"%s\",\"build\":\"%s\",\"slot\":\"%s\",\"address\":%u,\"slot_size\":%u,\"next_slot\":\"%s\",\"app_size\":%u,\"app_sha256\":\"%s\",\"table_sha256\":\"%s\",\"boot_sha256\":\"%s\",\"config_sha256\":\"%s\",\"state\":%d,\"pending\":%s,\"healthy\":%s,\"rollback_enabled\":true,\"active\":%s,\"offset\":%u,\"chunk_size\":1024,\"write_window\":4,\"binary_chunk_size\":4096,\"session\":\"%s\",\"storage_total\":%u,\"psram_total\":%u,\"led_order\":\"%s\",\"scan_color\":\"%s\"}\n",tx.c_str(),profile,MARAUDER_FIRMWARE_SEMVER,run->label,run->address,run->size,next?next->label:"",appSize,appHash.c_str(),tableHash.c_str(),bootHash.c_str(),settingsHash().c_str(),int(state),pending?"true":"false",healthy?"true":"false",handle?"true":"false",received,session.c_str(),(unsigned)storageTotalBytes(),ESP.getPsramSize(),MARAUDER_LED_DESCRIPTION,MARAUDER_SCAN_COLOR);kick();
}
}
void setup(){
 const auto* run=esp_ota_get_running_partition();appSize=ESP.getSketchSize();appHash=flashHash(run->address,appSize);tableHash=flashHash(0x8000,0x1000);bootHash=flashHash(0x2000,0x6000);configHash=settingsHash();
 healthy=layout()&&storageTotalBytes()==MARAUDER_STORAGE_SIZE&&ESP.getPsramSize()==0x800000&&configHash.length()==64&&appHash.length()==64;
 esp_ota_img_states_t state;if(esp_ota_get_state_partition(run,&state)==ESP_OK&&state==ESP_OTA_IMG_PENDING_VERIFY){pending=true;verifyDeadline=millis()+60000;host_connection::console().println("[OTA] Trial boot; waiting for USB verification (60 seconds)");}
}
bool active(){return handle||rebootAt;}
void tick(){
 uint32_t now=millis();if(int32_t(drainUntil-now)>0&&host_connection::console().availableForWrite()<2048){usb_serial_jtag_ll_txfifo_flush();usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);}
 if(handle&&now-lastActivity>15000){abortTransfer();host_connection::console().println("[OTA] Transfer expired; current boot slot preserved");}
 if(rebootAt&&int32_t(now-rebootAt)>=0)ESP.restart();
 if(pending&&int32_t(now-verifyDeadline)>=0){host_connection::console().println("[OTA] USB verification expired; rolling back");host_connection::console().flush();esp_ota_mark_app_invalid_rollback_and_reboot();ESP.restart();}
}
void command(const String& input){
 String a[8];int count=0,start=0;for(unsigned i=0;i<=input.length();i++)if(i==input.length()||input[i]==' '){if(i>unsigned(start)){if(count==8)return;a[count++]=input.substring(start,i);}start=i+1;}
 String tx=count>2?a[2]:"manual";if(!token(tx))return;String op=count>1?a[1]:"status";
 if(op=="status"){status(tx);return;}
 if(op=="confirm"){
  if(count!=4||a[3]!=appHash||!healthy){reply(tx,false,"self_test_or_image");return;}
  if(pending){if(esp_ota_mark_app_valid_cancel_rollback()!=ESP_OK){reply(tx,false,"confirm_failed");return;}pending=false;}
  reply(tx,true,"confirmed");return;
 }
 if(op=="begin"){
  uint32_t size=0;if(count!=7||!token(a[3])||!number(a[4],size)||size<288||size>MARAUDER_APP_SLOT_SIZE||!sha(a[5])||a[6]!=profile){reply(tx,false,"arguments_or_profile");return;}
  if(handle||committed){if(a[3]==session&&size==expectedSize&&a[5]==expected){lastActivity=millis();reply(tx,true,"already_started");}else reply(tx,false,"busy");return;}
  if(pending||!healthy||!layout()||wifi_scan_obj.scanning()||wifi_scan_obj.wifi_connected||device_observation::bleActive()){reply(tx,false,"not_ready");return;}
  target=esp_ota_get_next_update_partition(nullptr);if(!target||target==esp_ota_get_running_partition()||size>target->size){reply(tx,false,"partition");return;}
  if(esp_ota_begin(target,size,&handle)!=ESP_OK){handle=0;reply(tx,false,"begin_failed");return;}
  mbedtls_sha256_init(&digest);mbedtls_sha256_starts(&digest,0);session=a[3];expected=a[5];expected.toLowerCase();expectedSize=size;received=0;lastLength=0;lastActivity=millis();reply(tx,true,"started");return;
 }
 if(op=="abort") {if(count!=4||a[3]!=session||committed){reply(tx,false,"session");return;}abortTransfer();reply(tx,true,"aborted");return;}
 if(op=="write"||op=="writebin"){
  uint32_t offset=0,wantedCRC=0;if(count!=7||!handle||a[3]!=session||!number(a[4],offset)||!number(a[5],wantedCRC)){reply(tx,false,"session_or_arguments");return;}
  uint8_t decoded[1024];String binary;const uint8_t* b=decoded;size_t n=0;
  if(op=="writebin"){
   uint32_t length=0;if(!number(a[6],length)||!length||length>4096||offset>expectedSize||length>expectedSize-offset){reply(tx,false,"chunk_range");return;}
   binary=host_connection::readLegacyBytes(length,3000,String("@OTA:{\"tx\":\"")+tx+"\",\"ok\":true,\"code\":\"ready\"}");
   n=binary.length();b=(const uint8_t*)binary.c_str();if(n!=length){reply(tx,false,"binary_timeout");return;}
  }else if(mbedtls_base64_decode(decoded,sizeof(decoded),&n,(const unsigned char*)a[6].c_str(),a[6].length())){reply(tx,false,"chunk_encoding");return;}
  if(!n||offset>expectedSize||n>expectedSize-offset||crc(b,n)!=wantedCRC){reply(tx,false,"chunk_crc_or_range");return;}
  if(offset==lastOffset&&n==lastLength&&wantedCRC==lastCRC&&received==offset+n){lastActivity=millis();reply(tx,true,"duplicate");return;}
  if(offset!=received){reply(tx,false,"offset");return;}
  if(!received){if(n<sizeof(esp_image_header_t)){reply(tx,false,"header");return;}esp_image_header_t h;memcpy(&h,b,sizeof(h));if(h.magic!=ESP_IMAGE_HEADER_MAGIC||h.chip_id!=ESP_CHIP_ID_ESP32C5||h.spi_size!=MARAUDER_FLASH_SIZE_CODE){reply(tx,false,"chip_or_flash_header");return;}}
  if(esp_ota_write(handle,b,n)!=ESP_OK){abortTransfer();reply(tx,false,"write_failed");return;}
  mbedtls_sha256_update(&digest,b,n);lastOffset=offset;lastLength=n;lastCRC=wantedCRC;received+=n;lastActivity=millis();reply(tx,true,"written");return;
 }
 if(op=="commit"){
  if(count!=4||a[3]!=session){reply(tx,false,"session");return;}
  if(committed){reply(tx,true,"rebooting");return;}
  if(!handle||received!=expectedSize){reply(tx,false,"incomplete");return;}
  uint8_t hash[32];mbedtls_sha256_finish(&digest,hash);mbedtls_sha256_free(&digest);
  if(hex(hash,32)!=expected){esp_ota_abort(handle);handle=0;session="";reply(tx,false,"image_sha256");return;}
  esp_err_t result=esp_ota_end(handle);handle=0;
  if(result!=ESP_OK){session="";reply(tx,false,"image_invalid");return;}
  if(esp_ota_set_boot_partition(target)!=ESP_OK){session="";reply(tx,false,"select_failed");return;}
  committed=true;reply(tx,true,"rebooting");rebootAt=millis()+600;return;
 }
 reply(tx,false,"operation");
}
}
#else
namespace usb_ota {void setup(){} void tick(){} bool active(){return false;} void command(const String&) {}}
#endif

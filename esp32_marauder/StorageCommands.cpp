#include "HostConnection.h"
#include "StorageCommands.h"
#if defined(MARAUDER_INTERNAL_STORAGE)
#include "StorageBackend.h"
#include "StorageService.h"
#include "WiFiScan.h"
#include "DeviceObservability.h"
#include <mbedtls/base64.h>
#if ARDUINO_USB_CDC_ON_BOOT
#include "hal/usb_serial_jtag_ll.h"
#endif
extern WiFiScan wifi_scan_obj;
namespace storage_commands {
namespace {
uint32_t drainUntil=0;
void kick() {
 drainUntil=millis()+2000;
#if ARDUINO_USB_CDC_ON_BOOT
 usb_serial_jtag_ll_txfifo_flush();usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
#endif
}
String hex(const String& s){const char* h="0123456789abcdef";String r;r.reserve(s.length()*2);for(unsigned char c:s){r+=h[c>>4];r+=h[c&15];}return r;}
int nibble(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
bool path(const String& encoded,String& decoded){
 if(encoded.length()%2 || encoded.length()>320)return false;
 for(unsigned i=0;i<encoded.length();i+=2){int a=nibble(encoded[i]),b=nibble(encoded[i+1]);if(a<0||b<0)return false;char c=a*16+b;if(uint8_t(c)<32||uint8_t(c)==127||c=='\\')return false;decoded+=c;}
 if(!decoded.startsWith("/")||decoded.indexOf("//")>=0)return false;
 if(decoded.length()>1&&decoded.endsWith("/"))return false;
 String padded=decoded+"/";return padded.indexOf("/../")<0&&padded.indexOf("/./")<0;
}
bool uintArg(const String& s,uint32_t& n){if(s.isEmpty()||s.length()>9)return false;n=0;for(char c:s){if(c<'0'||c>'9')return false;n=n*10+c-'0';}return true;}
void error(const String& tx,const char* reason){host_connection::console().printf("File error: %s. Check file syntax, ls / and storageinfo; stopscan before changing files.\n",reason);host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"end\",\"ok\":false,\"error\":\"%s\"}\n",tx.c_str(),reason);kick();}
void done(const String& tx,const String& p=""){host_connection::console().println("File operation complete: "+p);host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"end\",\"ok\":true,\"path\":\"%s\"}\n",tx.c_str(),hex(p).c_str());kick();}
bool parents(const String& p){for(int i=1;i<int(p.length());i++)if(p[i]=='/'&&!storageFS().exists(p.substring(0,i))&&!storageFS().mkdir(p.substring(0,i)))return false;return true;}
uint32_t crc(const uint8_t* p,size_t n){uint32_t c=0xffffffff;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^((c&1)?0xedb88320:0);}return c^0xffffffff;}
}
void tick(){
#if ARDUINO_USB_CDC_ON_BOOT
 if(int32_t(drainUntil-millis())>0 && host_connection::console().availableForWrite()<2048){usb_serial_jtag_ll_txfifo_flush();usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);}
#endif
}
void command(const String& input){
 String a[8];int count=0,start=0;for(unsigned i=0;i<=input.length();i++)if(i==input.length()||input[i]==' '){if(i>unsigned(start)){if(count==8)return;a[count++]=input.substring(start,i);}start=i+1;}
 // file <action> <transaction> <hex-path> [offset|hex-destination] [limit]
 if(count<3)return;String tx=a[2];if(tx.isEmpty()||tx.length()>40)return;for(char c:tx)if(!isalnum(uint8_t(c))&&c!='_'&&c!='-')return;
 if(count<4){error(tx,"arguments");return;}
 storage_service::Request q{};String op=a[1];
 if(op=="list")q.op=storage_service::Op::List;else if(op=="stat")q.op=storage_service::Op::Stat;
 else if(op=="read")q.op=storage_service::Op::Read;else if(op=="rename")q.op=storage_service::Op::Rename;
 else if(op=="mkdir")q.op=storage_service::Op::Mkdir;else if(op=="trash")q.op=storage_service::Op::Trash;
 else if(op=="restore")q.op=storage_service::Op::Restore;
 else if(op=="delete")q.op=storage_service::Op::Delete;else if(op=="emptytrash")q.op=storage_service::Op::EmptyTrash;
 else {error(tx,"operation");return;}
 if((op=="delete"||op=="emptytrash")&&count!=4){error(tx,"arguments");return;}
 if(!path(a[3],q.path)){error(tx,"path");return;}
 if(op=="rename"&&(count!=5||!path(a[4],q.destination))){error(tx,"path");return;}
 if((op=="list"&&count>4)||op=="read")if(!uintArg(a[4],q.offset)){error(tx,"offset");return;}
 if(op=="read"&&(count!=6||!uintArg(a[5],q.length))){error(tx,"range");return;}
 storage_service::Result r;storage_service::execute(q,r);if(!r.ok()){error(tx,r.error);return;}
 if(op=="list"){
  for(unsigned i=0;i<r.count;i++){auto& e=r.entries[i];host_connection::console().printf("%s%s  %u bytes\n",e.name.c_str(),e.directory?"/":"",e.size);host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"entry\",\"name\":\"%s\",\"directory\":%s,\"size\":%u}\n",tx.c_str(),hex(e.name).c_str(),e.directory?"true":"false",e.size);kick();}
  host_connection::console().printf("Listed %u entries; next offset %d\n",r.count,r.next);
  host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"end\",\"ok\":true,\"next\":%d,\"total\":%u,\"used\":%u}\n",tx.c_str(),r.next,r.total,r.used);kick();return;
 }
 if(op=="stat"){host_connection::console().printf("File size: %u bytes\n",r.size);host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"end\",\"ok\":true,\"size\":%u}\n",tx.c_str(),r.size);kick();return;}
 if(op=="read"){
  host_connection::console().printf("Read %u bytes at offset %u; total %u bytes\n",r.length,r.offset,r.size);
  unsigned char encoded[1369];size_t n=0;if(mbedtls_base64_encode(encoded,sizeof(encoded),&n,r.data,r.length)){error(tx,"encode");return;}encoded[n]=0;
  host_connection::console().printf("@FILE:{\"tx\":\"%s\",\"kind\":\"end\",\"ok\":true,\"offset\":%u,\"size\":%u,\"crc32\":%u,\"data\":\"%s\"}\n",tx.c_str(),r.offset,r.size,crc(r.data,r.length),encoded);kick();return;
 }
 done(tx,r.path);
}
}
#else
namespace storage_commands {void tick(){} void command(const String&) {}}
#endif

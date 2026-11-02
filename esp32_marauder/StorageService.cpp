#include "StorageService.h"
#include "StorageBackend.h"
#include "WiFiScan.h"
#include "DeviceObservability.h"
#include "HostConnection.h"
#include "StorageRemovalPosix.h"
extern WiFiScan wifi_scan_obj;
namespace storage_service {
namespace { bool lease=false; uint32_t revision=1;
bool parents(const String& p){for(unsigned i=1;i<p.length();i++)if(p[i]=='/'&&!storageFS().exists(p.substring(0,i))&&!storageFS().mkdir(p.substring(0,i)))return false;return true;}
struct RemovalFilesystem:storage_removal::PosixFilesystem {
 explicit RemovalFilesystem(const char* mount):PosixFilesystem(mount){}
 uint32_t started=millis();
 bool proceed(){yield();return uint32_t(millis()-started)<10000;}
};
}
bool leased(){return lease;}
uint32_t generation(){return revision;}
bool validPath(const String& p){
 if(!p.length()||p.length()>160||p[0]!='/'||p.indexOf("//")>=0||(p.length()>1&&p.endsWith("/")))return false;
 for(unsigned char c:p)if(c<32||c==127||c=='\\')return false;
 String padded=p+"/";return padded.indexOf("/../")<0&&padded.indexOf("/./")<0;
}
const char* available(bool owner){
 if(lease&&!owner)return "transfer_active";
 if(!owner&&host_connection::serviceBusy())return "busy";
 if(wifi_scan_obj.scanning()||wifi_scan_obj.wifi_connected||device_observation::bleActive())return "busy";
 if(!storageTotalBytes())return "unmounted";
 return nullptr;
}
const char* openStream(const String& path, File& file,uint32_t& gen){
 if(const char* e=available())return e;
 if(!validPath(path))return "path";
 file=storageFS().open(path,FILE_READ);
 if(!file||file.isDirectory()){file.close();return "not_file";}
 lease=true;gen=revision;return nullptr;
}
void closeStream(File& file){file.close();lease=false;}
void execute(const Request& q,Result& r){
 r.error=available();if(r.error)return;
 if(!validPath(q.path)){r.error="path";return;}auto& fs=storageFS();const String& p=q.path;
 if(q.op==Op::Delete||q.op==Op::EmptyTrash){
  #ifdef MARAUDER_INTERNAL_STORAGE
  RemovalFilesystem removal{STORAGE_INTERNAL_MOUNT};auto result=storage_removal::run(removal,p.c_str(),q.op==Op::EmptyTrash);
  if(result.removed)++revision;r.error=result.error;r.path=p;r.total=storageTotalBytes();r.used=storageUsedBytes();return;
  #else
  r.error="unsupported";return;
  #endif
 }
 if(q.op==Op::List){
  File dir=fs.open(p);if(!dir||!dir.isDirectory()){r.error="not_directory";return;}
  uint32_t skipped=0;File f=dir.openNextFile();
  while(f){String name=f.name();if(p!="/"||name!=".trash")if(skipped++>=q.offset){
   if(r.count==20){r.next=q.offset+r.count;f.close();break;}
   r.entries[r.count++]={name,f.isDirectory(),uint32_t(f.size())};
  }f.close();f=dir.openNextFile();yield();}
  dir.close();r.total=storageTotalBytes();r.used=storageUsedBytes();return;
 }
 if(q.op==Op::Stat||q.op==Op::Read){
  File f=fs.open(p,FILE_READ);if(!f||f.isDirectory()){r.error="not_file";return;}r.size=f.size();
  if(q.op==Op::Read){
   if(!q.length||q.length>sizeof(r.data)||q.offset>r.size){r.error="range";return;}
   r.offset=q.offset;r.length=std::min<uint32_t>(q.length,r.size-q.offset);
   if(!f.seek(q.offset)||f.read(r.data,r.length)!=r.length)r.error="read";
  }f.close();return;
 }
 if(p=="/"||p=="/.trash"){r.error="protected_path";return;}
 if(q.op==Op::Mkdir){
  if(p.startsWith("/.trash/")||fs.exists(p)){r.error="exists_or_protected";return;}
  if(!fs.mkdir(p)){r.error="mkdir";return;}r.path=p;++revision;return;
 }
 String dest;
 if(q.op==Op::Rename){dest=q.destination;if(!validPath(dest)||dest=="/"||dest=="/.trash"||dest.startsWith("/.trash/")||p.startsWith("/.trash/")){r.error="path";return;}}
 else if(q.op==Op::Trash){if(p.startsWith("/.trash/")){r.error="protected_path";return;}dest="/.trash"+p;}
 else if(q.op==Op::Restore){if(!p.startsWith("/.trash/")){r.error="path";return;}dest=p.substring(7);}
 else {r.error="operation";return;}
 if(!validPath(dest)||dest.startsWith(p+"/")||!fs.exists(p)||fs.exists(dest)){r.error="missing_or_exists";return;}
 if((q.op==Op::Trash||q.op==Op::Restore)&&!parents(dest)){r.error="mkdir";return;}
 if(!fs.rename(p,dest)){r.error="rename";return;}r.path=dest;++revision;
}
}

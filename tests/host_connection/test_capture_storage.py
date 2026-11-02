"""Run the actual Buffer implementation against a counting filesystem."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as td:
 p=Path(td)
 for f in ['Buffer.cpp','Buffer.h','PcapHeader.h','PcapHeader.cpp']:(p/f).write_bytes((ROOT/'esp32_marauder'/f).read_bytes())
 (p/'Arduino.h').write_text('''#pragma once
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdint>
struct String:std::string{using std::string::string;String(const std::string& s):std::string(s){}String(int n):std::string(std::to_string(n)){}operator const char*()const{return c_str();}};
inline uint32_t millis(){return 10000;}inline uint32_t micros(){return 10000000;}inline void delay(int){}
''')
 (p/'FS.h').write_text('''#pragma once
#include "Arduino.h"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
inline size_t diskBytes=0,diskWrites=0,creates=0;
struct File{operator bool()const{return true;}void close(){}size_t write(const uint8_t*,size_t n){diskBytes+=n;diskWrites++;return n;}};
namespace fs{struct FS{bool exists(const String&){return false;}File open(const String&,const char* mode){if(*mode=='w')creates++;return {};}};}
''')
 (p/'settings.h').write_text('''#pragma once
inline bool savePcap=true;struct Settings{template<class T>T loadSetting(const char*){return savePcap;}};
''')
 (p/'configs.h').write_text('#pragma once\n#define BUF_SIZE 8192\n#define SNAP_LEN 2324\n#define MARAUDER_INTERNAL_STORAGE 1\n')
 (p/'esp_wifi_types.h').write_text('#pragma once\nstruct wifi_promiscuous_pkt_t{uint8_t payload[2324];};\n')
 (p/'lang_var.h').write_text('#pragma once\ninline const char* text02="error";inline const char* text_table4[]={"","","","","","","","SavePCAP"};\n')
 (p/'StorageBackend.h').write_text('#pragma once\ninline bool storageHasSpace(size_t){return true;}\n')
 (p/'HostConnection.h').write_text('''#pragma once
#include "Arduino.h"
#include "esp_wifi_types.h"
#define HOST_CONNECTION_ENABLED 1
namespace host_connection{inline unsigned accepted=0;inline void captureAccepted(const wifi_promiscuous_pkt_t*){accepted++;}inline bool running=false;inline bool capturing(){return running;}inline bool active(){return running;}struct Out{template<class T>void println(T){}size_t write(const uint8_t*,size_t n){return n;}};inline Out& console(){static Out x;return x;}}
''')
 (p/'main.cpp').write_text('''#include "Arduino.h"
#include "FS.h"
#include "HostConnection.h"
#define private public
#include "Buffer.h"
#undef private
#include <cassert>
#include <iostream>
Settings settings_obj;
int main(){Buffer b;fs::FS fs;wifi_promiscuous_pkt_t packet{};
 b.pcapOpen("test",&fs,false);b.append(&packet,100);b.save(true);auto before=diskBytes;assert(before>24);
 b.append(&packet,100);b.suppressLocalPcap();assert(!b.bufSizeA&&!b.bufSizeB);host_connection::running=true;
 for(int i=0;i<10000;i++){b.append(&packet,100);b.save(true);}assert(diskBytes==before);
 host_connection::running=false;for(int i=0;i<10000;i++){b.append(&packet,100);b.save(true);}assert(diskBytes==before); // stop/disconnect cannot resume local PCAP
 auto creations=creates;auto bytesBefore=diskBytes;
 b.prepareHostOnly(true);b.pcapOpen("hostonly",&fs,false);b.prepareHostOnly(false);
 b.append(&packet,100);b.save(true);assert(creates==creations&&diskBytes==bytesBefore); // no empty PCAP/header either
 b.pcapOpen("nextscan",&fs,false);b.append(&packet,100);b.save(true);assert(diskBytes>before&&creates==2);
 auto accepted=host_connection::accepted;savePcap=false;b.append(&packet,100);assert(host_connection::accepted==accepted+1);free(b.bufA);free(b.bufB);std::cout<<"PASS: actual Buffer; suppress discards pending PCAP, 20000 appends never reach filesystem even after stream stops, next capture restores normal policy"<<std::endl;
}
''')
 subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined','-g','-I',str(p),str(p/'Buffer.cpp'),str(p/'PcapHeader.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

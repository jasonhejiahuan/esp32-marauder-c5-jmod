#include "HostConnection.h"
#include "Buffer.h"
#include "PcapHeader.h"
#include "lang_var.h"
#include "StorageBackend.h"

Buffer::Buffer(){
  bufA = (uint8_t*)malloc(BUF_SIZE);
  bufB = (uint8_t*)malloc(BUF_SIZE);
}

void Buffer::createFile(const char* name, bool is_pcap, bool is_gpx){
  int i=0;
  String prefix = directory ? String(directory) + "/" : "/";
  if (is_pcap) {
    do{
      fileName = prefix+String(name)+"_"+(String)i+".pcap";
      i++;
    } while(fs->exists(fileName));
  }
  else if ((!is_pcap) && (!is_gpx)) {
    do{
      fileName = prefix+String(name)+"_"+(String)i+".log";
      i++;
    } while(fs->exists(fileName));
  }
  else {
    do{
      fileName = prefix+String(name)+"_"+(String)i+".gpx";
      i++;
    } while(fs->exists(fileName));
  }

  host_connection::console().println(fileName);
  
  file = fs->open(fileName, FILE_WRITE);
  file.close();
}

void Buffer::open(bool is_pcap){
  bufSizeA = 0;
  bufSizeB = 0;

  bufSizeB = 0;

  writing = true;

  if (is_pcap) {
    uint8_t header[marauder::kPcapGlobalHeaderSize];
    marauder::makePcapGlobalHeader(SNAP_LEN, header);
    write(header, sizeof(header));
  }
}

String Buffer::getFileName() {
  return this->fileName;
}

void Buffer::setDirectory(const char* path) {
  directory = path;
}

// Session-only suppression: never change persistent SavePCAP configuration.
void Buffer::suppressLocalPcap(){
  pcapSuppressed=true;
  if(pcapFile){writing=false;bufSizeA=bufSizeB=0;file.close();this->fs=nullptr;this->serial=false;}
}
void Buffer::openFile(const char* file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx) {
  pcapFile=is_pcap;pcapSuppressed=false;
  if(is_pcap && hostOnlyOpen){suppressLocalPcap();return;}
  bool save_pcap = settings_obj.loadSetting<bool>("SavePCAP");
  if (!save_pcap) {
    this->fs = NULL;
    this->serial = false;
    writing = false;
    return;
  }
  this->fs = fs;
  this->serial = serial;
  if (this->fs) {
    createFile(file_name, is_pcap, is_gpx);
  }
  if (this->fs || this->serial) {
    open(is_pcap);
  } else {
    writing = false;
  }
}

void Buffer::pcapOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, true);
}

void Buffer::logOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false);
}

void Buffer::gpxOpen(const char* file_name, fs::FS* fs, bool serial) {
  openFile(file_name, fs, serial, false, true);
}

void Buffer::add(const uint8_t* buf, uint32_t len, bool is_pcap){
  if((is_pcap&&pcapSuppressed)||host_connection::capturing())return;
  // buffer is full -> drop packet
  if((useA && bufSizeA + len >= BUF_SIZE && bufSizeB > 0) || (!useA && bufSizeB + len >= BUF_SIZE && bufSizeA > 0)){
    //host_connection::console().print(";");
    return;
  }
  
  if(useA && bufSizeA + len + 16 >= BUF_SIZE && bufSizeB == 0){
    useA = false;
    //host_connection::console().println("\nswitched to buffer B");
  }
  else if(!useA && bufSizeB + len + 16 >= BUF_SIZE && bufSizeA == 0){
    useA = true;
    //host_connection::console().println("\nswitched to buffer A");
  }

  uint32_t microSeconds = micros(); // e.g. 45200400 => 45s 200ms 400us
  uint32_t seconds = (microSeconds/1000)/1000; // e.g. 45200400/1000/1000 = 45200 / 1000 = 45s

  microSeconds -= seconds*1000*1000; // e.g. 45200400 - 45*1000*1000 = 45200400 - 45000000 = 400us (because we only need the offset)
  
  if (is_pcap) {
    write(seconds); // ts_sec
    write(microSeconds); // ts_usec
    write(len); // incl_len
    write(len); // orig_len
  }
  
  write(buf, len); // packet payload
}

void Buffer::append(wifi_promiscuous_pkt_t *packet, int len) {
#ifdef HOST_CONNECTION_ENABLED
  // Native sniff predicates have accepted this frame. Stream before SavePCAP
  // settings or storage batching; the RX callback itself never calls this path.
  host_connection::captureAccepted(packet);
#endif
  bool save_packet = settings_obj.loadSetting<bool>(text_table4[7]);
  if (save_packet) {
    add(packet->payload, len, true);
  }
}

void Buffer::append(String log) {
  bool save_packet = settings_obj.loadSetting<bool>(text_table4[7]);
  if (save_packet) {
    add((const uint8_t*)log.c_str(), log.length(), false);
  }
}

void Buffer::write(int32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint16_t n){
  uint8_t buf[2];
  buf[0] = n;
  buf[1] = n >> 8;
  write(buf,2);
}

void Buffer::write(const uint8_t* buf, uint32_t len){
  if(!writing) return;
  while(saving) delay(10);
  
  if(useA){
    memcpy(&bufA[bufSizeA], buf, len);
    bufSizeA += len;
  }else{
    memcpy(&bufB[bufSizeB], buf, len);
    bufSizeB += len;
  }
}

void Buffer::saveFs(){
  if (!storageHasSpace(bufSizeA + bufSizeB)) {
    host_connection::console().println("[Storage] Full; capture file output stopped, CLI remains available");
    this->fs = nullptr;
    return;
  }
  file = fs->open(fileName, FILE_APPEND);
  if (!file) {
    host_connection::console().println(text02+fileName+"'");
    this->fs = nullptr;
    return;
  }

  bool ok = true;
  if(useA){
    if(bufSizeB > 0){
      ok = ok && file.write(bufB, bufSizeB) == bufSizeB;
    }
    if(bufSizeA > 0){
      ok = ok && file.write(bufA, bufSizeA) == bufSizeA;
    }
  } else {
    if(bufSizeA > 0){
      ok = ok && file.write(bufA, bufSizeA) == bufSizeA;
    }
    if(bufSizeB > 0){
      ok = ok && file.write(bufB, bufSizeB) == bufSizeB;
    }
  }

  file.close();
  if (!ok) { host_connection::console().println("[Storage] Short write; capture file output stopped"); this->fs = nullptr; }
}

void Buffer::saveSerial() {
  // Host_Connection raw capture has its own binary channel and fixed pool.
  // Never tunnel legacy PCAP marker/data bytes through the framed text console.
  if(host_connection::active())return;
  // Saves to main console UART, user-facing app will ignore these markers
  // Uses / and ] in markers as they are illegal characters for SSIDs
  const char* mark_begin = "[BUF/BEGIN]";
  const size_t mark_begin_len = strlen(mark_begin);
  const char* mark_close = "[BUF/CLOSE]";
  const size_t mark_close_len = strlen(mark_close);

  // Additional buffer and memcpy's so that a single host_connection::console().write() is called
  // This is necessary so that other console output isn't mixed into buffer stream
  uint8_t* buf = (uint8_t*)malloc(mark_begin_len + bufSizeA + bufSizeB + mark_close_len);
  uint8_t* it = buf;
  memcpy(it, mark_begin, mark_begin_len);
  it += mark_begin_len;

  if(useA){
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
  } else {
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
  }

  memcpy(it, mark_close, mark_close_len);
  it += mark_close_len;
  host_connection::console().write(buf, it - buf);
  free(buf);
}

void Buffer::save(bool force) {
  if((pcapFile&&pcapSuppressed)||host_connection::capturing())return;
  #ifdef MARAUDER_INTERNAL_STORAGE
    // Batch small writes; no change to the USB transport or SD-card profiles.
    if (!force && millis() - lastSaveMs < 100 && bufSizeA + bufSizeB < BUF_SIZE / 2) return;
    lastSaveMs = millis();
  #endif
  saving = true;

  if((bufSizeA + bufSizeB) == 0){
    saving = false;
    return;
  }

  if(this->fs) saveFs();
  if(this->serial) saveSerial();

  bufSizeA = 0;
  bufSizeB = 0;

  saving = false;
}

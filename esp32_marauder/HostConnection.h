#pragma once
#include <Arduino.h>
#include "configs.h"
#include "HostConnectionCodec.h"
#if defined(MARAUDER_C5) && (defined(MARAUDER_WAVESHARE_C5_RGB) || defined(MARAUDER_XIAO_C5)) && defined(MARAUDER_INTERNAL_STORAGE)
#define HOST_CONNECTION_ENABLED 1
#include <esp_wifi.h>
namespace host_connection {
class Output : public Print {
 public: using Print::write;size_t write(uint8_t c) override;size_t write(const uint8_t* p,size_t n) override;
 int availableForWrite();void flush();
 uint32_t commandBytes() const {return command_bytes_;}
 private: uint32_t command_bytes_=0;
};
Output& console();
String readLegacyInput(size_t limit,uint32_t timeout);
String readLegacyBytes(size_t length,uint32_t timeout,const String& readyLine);
void begin();void execute();void idle();
bool initialized();bool active();bool capturing();bool serviceBusy();
void discovery(const char* transaction);
void capture(void* packet,wifi_promiscuous_pkt_type_t type);
void captureAccepted(const wifi_promiscuous_pkt_t* packet);
void setLegacyCallback(wifi_promiscuous_cb_t cb);
void receive(void* packet,wifi_promiscuous_pkt_type_t type);
}
#else
namespace host_connection {
inline auto& console(){return Serial;}
inline bool active(){return false;}inline bool capturing(){return false;}inline bool serviceBusy(){return false;}
inline void begin(){}inline void execute(){}inline void idle(){}
}
#endif

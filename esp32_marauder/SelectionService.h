#pragma once
#include "HostConnectionCodec.h"
#include "DiscoveryFrame.h"
namespace selection_service {
const char* list(host_connection::Reader&,host_connection::Writer&);
const char* set(host_connection::Reader&,host_connection::Writer&);
// Executor-only discovery registration; never selects a device or resets its index.
const char* observe(const discovery::Observation&,uint8_t channel,int8_t rssi,uint32_t at);
void begin();
void settle();
void discardDeferred();
}

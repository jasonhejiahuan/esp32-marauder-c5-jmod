#pragma once
#include <stdint.h>
#include <esp_wifi.h>
namespace combined {
bool begin();
void end();
void receive(void*,wifi_promiscuous_pkt_type_t);
void pump(uint32_t);
}

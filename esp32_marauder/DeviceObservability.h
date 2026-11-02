#pragma once
#include "HostConnectionCodec.h"
#include "configs.h"
#include <Arduino.h>
namespace device_observation {
void binaryReport(host_connection::Writer&);
void setup();
void report(const char* transaction);
bool bleActive();
void bleCommand(const String& action);
void stop();
void tick();
}

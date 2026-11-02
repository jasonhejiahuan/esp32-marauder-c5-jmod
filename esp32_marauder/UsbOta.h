#pragma once
#include <Arduino.h>
namespace usb_ota {
void setup();
void tick();
bool active();
void command(const String& input);
}

#include "configs.h"
#if defined(MARAUDER_XIAO_C5)
#include <Arduino.h>

// Compile against Seeed's native variant; do not replace its pin declarations.
static_assert(D0 == 1 && D1 == 0 && D2 == 25 && D3 == 7);
static_assert(D4 == 23 && D5 == 24 && D6 == 11 && D7 == 12);
static_assert(D8 == 8 && D9 == 9 && D10 == 10);
static_assert(SDA == 23 && SCL == 24 && TX == 11 && RX == 12);
static_assert(SS == 7 && SCK == 8 && MISO == 9 && MOSI == 10);
static_assert(LED_BUILTIN == 27 && BAT_VOLT_PIN == 6 && BAT_VOLT_PIN_EN == 26);
#ifdef HAS_GPS
// Optional GPS can use the otherwise unclaimed D0/D1/D2 pads. It must not
// re-route the native USB, UART0, SPI, I2C, JTAG, battery or flash signals.
constexpr bool gpsPad(int pin) { return pin == D0 || pin == D1 || pin == D2; }
static_assert(gpsPad(MARAUDER_XIAO_GPS_RX) && gpsPad(MARAUDER_XIAO_GPS_TX));
static_assert(MARAUDER_XIAO_GPS_RX != MARAUDER_XIAO_GPS_TX);
#endif
#endif

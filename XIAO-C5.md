# Seeed Studio XIAO ESP32-C5

Board profile: `xiao-c5-usb-3m1664k-v1`, introduced in `1.18.0-jmod2.6.0`; single-USB transport in `1.18.0-jmod2.6.1`.
Use the official Arduino-ESP32 3.3.6 `XIAO_ESP32C5` variant, native USB CDC,
8 MiB Flash and 8 MiB PSRAM. This is a separate target from Waveshare C5.

## Native pins

No Arduino core or `pins_arduino.h` changes are required. `XiaoC5Pins.cpp`
checks the official declarations at compile time.

| Signal | XIAO pad | GPIO |
|---|---|---:|
| General / ADC | D0 | 1 |
| General | D1 | 0 |
| General | D2 | 25 |
| SPI SS / general | D3 | 7 |
| I²C SDA / SCL | D4 / D5 | 23 / 24 |
| UART TX / RX | D6 / D7 | 11 / 12 |
| SPI SCK / MISO / MOSI | D8 / D9 / D10 | 8 / 9 / 10 |
| Native USB | USB-C | 13 / 14 |
| User LED (active low, yellow) | onboard | 27 |
| Battery sense / enable | onboard | 6 / 26 |
| Boot | onboard | 28 |

Host_Connection uses only the native USB interface. UART0 is not initialized
as a second host port; TX/RX pads retain their native definitions for peripherals.
No UART baud negotiation or dual-link service is advertised or enabled on XIAO.
USB CDC does not need high UART baud rates. I²C and SPI
declarations remain native; the shipped profile does not initialize absent
external peripherals. JTAG pads and battery-control wiring are not repurposed.

There is no onboard RGB LED, GPS, screen or SD socket. LED status and the
`EnableLED` setting use the yellow LED. `led -s FFFFFF` turns it on and
`led -s 000000` turns it off; rainbow reports the hardware limitation.
Since jmod2.6.2, idle without an active Host_Connection session blinks yellow
(one second on, one second off); an active session keeps it solid yellow.
`EnableLED=false` disables both indications, and explicit custom LED commands
retain control until the mode changes. This single-color LED cannot emit white.
The Waveshare RGB profile displays solid white while connected and idle.
Dashboard validates each board's matching hardware/profile pair.

An optional external GPS build may define both `MARAUDER_XIAO_GPS_RX` and
`MARAUDER_XIAO_GPS_TX` using distinct D0/D1/D2 GPIO values. These refer to
MCU receive/transmit pins. The build rejects conflicts with native buses.
No GPS is probed and no extra header pads are claimed in the default build.

The red charging LED is separate from the yellow user LED. The V1.1 schematic
connects it to VBAT through R14 and to the charger's NCHG output, not to an ESP32
GPIO. Its charging/full indications are controlled by the SGM40567; the
schematic specifies a 51.2-second indication after charging completes. Red
LED extinction is not evidence that firmware disabled a GPIO.
External SD hardware requires an explicit peripheral profile; this build
uses its internal filesystem without claiming the SPI pads.

## Flash layout

| Partition | Offset | Size |
|---|---:|---:|
| NVS | `0x009000` | 20 KiB |
| OTA metadata | `0x00E000` | 8 KiB |
| app0 | `0x010000` | 3 MiB |
| app1 | `0x310000` | 3 MiB |
| Settings SPIFFS | `0x610000` | 256 KiB |
| Core dump | `0x650000` | 64 KiB |
| Files LittleFS | `0x660000` | 1664 KiB |

The last partition ends at exactly `0x800000`. Filesystem metadata consumes
some of the data area. Both settings and files are formatted only when their
expected partitions are verified blank; a failed mount of existing data does
not trigger an automatic format. USB OTA updates only the inactive app slot.

## Build and update

```sh
python3 tools/build_xiao_c5.py --build-path .build/xiao-c5
python3 tools/package_usb_ota.py .build/xiao-c5/build releases/xiao-c5
python3 tools/usb_update.py releases/xiao-c5 --port YOUR_SERIAL_PORT
```

The build script uses `arduino-cli` on PATH and its normal configuration by
default. Pass `--arduino-cli` and optionally `--config-file` to select a
different installation. An optional local `discovery-build.json` supplies
defaults for existing setups and is excluded from Git.
Packaging/updating requires Python with esptool and pyserial installed.
The build copies source into an isolated directory and replaces only that
copy's partition table. The existing Waveshare `partitions.csv` stays intact.

For **first installation only**, back up the actual device's entire flash
once, verify its size/hash and chip identity, then install the generated
`esp32_marauder.ino.merged.bin` at address zero using esptool with
`--flash-mode keep --flash-size keep`. This 8 MiB first-install image resets
the device's previous contents. Do not use it for routine updates.

Routine updates use the application bundle above, preserve settings/files,
and require a matching profile, bootloader hash and partition hash. A
Waveshare bundle is rejected. The updater verifies the trial slot over USB
before confirming it; an unconfirmed trial is subject to rollback.

## Features and verification boundary

The C5 radio/CLI code, dual-band scanning, BLE observation, CSI motion,
Host_Connection, file operations and dual-slot update protocol are retained.
The board-specific changes are pin selection, LED handling, storage geometry
and initialization. Radio operations requiring external peers, wired
peripherals, or additional adapters need their own acceptance evidence.

References: [Seeed getting started and pin map](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
and the installed official Arduino `variants/XIAO_ESP32C5/pins_arduino.h`.

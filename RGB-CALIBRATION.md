# Waveshare C5 RGB calibration

Board: Waveshare ESP32-C5-WIFI6-KIT-N32R8-U.

The previous NEO_GRB encoding exchanges red and green on this board. Build it with both `-DMARAUDER_C5` and `-DMARAUDER_WAVESHARE_C5_RGB` to select NEO_RGB at the shared Adafruit_NeoPixel constructor. Other board builds retain their original order. See `tools/build_host_connection.py` for the build arguments.

The constructor covers setColor, scan/status modes, rainbow/Wheel and CLI custom colors. No per-mode channel swap is needed; do not swap those values again. GPIO27 and brightness 50 remain unchanged. The AP + STA + Beacon color is 80FFFF (cyan-white). CHG and PWR are separate hardware indicators and are not firmware-controlled.

Before first installation, preserve and verify an appropriate backup. Compatible routine OTA updates use the inactive application slot; partitions and bootloader remain unchanged. The older official updater does not preserve the combined mode or this board calibration flag automatically.

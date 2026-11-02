# Waveshare C5 internal application storage

This board profile uses a real LittleFS volume in onboard NOR flash. It is not an SD-card emulator and does not expose a USB disk. `storageFS()` provides the existing Arduino filesystem interface to application file operations. Physical SD initialization is disabled for this profile.

## Current layout

The initial 2 MiB layout below is historical. The current board has **two 4 MiB app slots and 6 MiB LittleFS**, with configuration migrated intact. See [USB-OTA.md](USB-OTA.md) for exact offsets, upgrade commands and verified boundaries.

## Original layout and build (superseded)

Physical flash was measured as 32 MiB. A full read showed the region from 8 MiB to 32 MiB erased before this change. The original seven partitions, including both OTA slots, NVS, existing SPIFFS, and coredump, retain their offsets and sizes. `esp32_marauder/partitions.csv` appends the independent `storage` partition at **0x800000**, size **0x200000 (2 MiB)**. The remaining 22 MiB is unallocated. The ESP-IDF partition subtype is `spiffs`, which LittleFS also uses; its distinct label is `storage`.

This historical layout used Arduino-ESP32 3.3.6 and a 16 MiB flash image setting. That window covers the allocated partitions without changing the physical 32 MiB flash size. Application slots were 0x330000 bytes each. Do not use this superseded partition table for a different board without checking its layout.

Required flags: `MARAUDER_C5`, `MARAUDER_WAVESHARE_C5_RGB`, `MARAUDER_INTERNAL_STORAGE`; native USB CDC enabled, PSRAM enabled. GPS remains disabled on GPIO13/14. RGB channel calibration and `scancombined` color #80FFFF are retained.

## Behavior

- On first boot, initialization formats **only the exactly matched new partition and only after verifying every byte is erased**. An existing volume that fails to mount is preserved, not reformatted.
- Existing application filesystem users route through `storageFS()`: saved lists, scripts, asset lookup, capture files, Recon records and the existing SPIFFS backup helpers. This does not add absent GPS/Bluetooth hardware or manufacture missing assets.
- `storageinfo` reports backend, mount state, total/used/free bytes. `info` identifies internal storage and explicitly says no SD card is present. `ls /` lists application files.
- Capture writes are batched up to 100 ms or half the existing buffer size; `stopscan` forces pending capture data out. Small pending data may be lost on sudden power removal. No claim of lossless capture or hard real-time latency is made.
- Capture file output stops if less than its pending data plus 64 KiB remains, or if a write fails; existing files are not automatically deleted. Recon record files close on a short write. The small volume can fill quickly during capture.
- Original settings remain in the original SPIFFS partition. Internal application storage is separate.

## Verified / limits

Filesystem exhaustion, abrupt power-loss recovery and every SD-dependent workflow have not been exhaustively tested.

The ESP32-C5 USB controller is fixed-function Serial/JTAG, so firmware cannot turn it into USB MSC or a custom generic USB endpoint. The Mac backend owns one native CDC connection and broadcasts cached data to multiple subscribers; writes remain queued. Hardware information: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c5/api-guides/usb-serial-jtag-console.html

## Upgrade / recovery

Keep this board profile, custom partition table and storage adapter when upgrading. The older updater that reapplies only the USB CDC patch does **not** preserve all these custom features; do not use its default build as an equivalent upgrade. App-only upgrades using the same layout preserve the data volume. An erase-all destroys it.

Keep any full-flash backup private: it may contain device identifiers and user data. Restore requires re-verifying board identity, backup hash, current flash and intended rollback state before writing.

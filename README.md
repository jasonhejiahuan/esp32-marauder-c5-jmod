# esp32-marauder-c5-jmod

JASON Studio's [ESP32 Marauder](https://github.com/justcallmekoko/ESP32Marauder) fork for **Waveshare ESP32-C5-WIFI6-KIT-N32R8-U** and **Seeed Studio XIAO ESP32-C5**, with **Marauder UI** (Marauder Desk), a local Python/browser dashboard.

This repository focuses on these two C5 build profiles, their USB host protocol, internal storage and dual-slot application updates. It is based on upstream **v1.18.0** with independent **JMOD 2.6.2** modifications; it is not an official upstream release.

## Two build targets

| Target | Flash / PSRAM | Connection | Status LED | Application slots / file storage |
|---|---|---|---|---|
| **Waveshare ESP32-C5-WIFI6-KIT-N32R8-U** | 32 MiB / 8 MiB | Native USB; optional UART control/data link | RGB, white while connected and idle | Two 4 MiB app slots / 6 MiB LittleFS |
| **Seeed Studio XIAO ESP32-C5** | 8 MiB / 8 MiB | **One native USB endpoint**; no UART baud negotiation or second-link binding | Yellow: slow blink when disconnected, solid while connected and idle | Two 3 MiB app slots / 1664 KiB LittleFS |

Each target has a distinct build identity and partition layout. Firmware packages are not interchangeable. The XIAO profile uses the official Arduino board variant and preserves its native header pin definitions; it does not claim absent GPS, SD or display hardware. Its red charge indicator is controlled by the charging circuit. `EnableLED` controls the firmware status indication.

- [XIAO build, pin map and partition layout](XIAO-C5.md)
- [Waveshare partition layout and USB OTA](USB-OTA.md)
- [Internal storage](INTERNAL-STORAGE.md) and [Waveshare RGB configuration](RGB-CALIBRATION.md)
- [Build dependencies and commands](docs/BUILDING.md)

## Marauder UI

[Marauder UI](Marauder-UI/README.md), displayed as **Marauder Desk**, runs locally and supports both profiles. It provides WLAN and BLE observation, device information, file management, captures and a CLI console over the JMOD Host_Connection protocol. It verifies the board's matching hardware/profile identity before accepting a connection.

The frontend uses local HTML/CSS/JavaScript and the Python service owns the serial port. No cloud account, remote fonts or CDN is required. The service listens on `127.0.0.1` and does not add telemetry or an upload service.

```sh
cd Marauder-UI
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
.venv/bin/python server.py
```

Open <http://127.0.0.1:8765> and select the intended board's serial port. On macOS, `Marauder-UI/启动.command` can launch an existing compatible Python environment.

## Firmware and updates

The fork retains the C5 radio/CLI features and adds typed host services, continuous AP/STA/Beacon observation, device metrics, internal file operations and application-only USB OTA. GPS-dependent functions require a suitable external peripheral profile.

With Arduino-ESP32 **3.3.6** and the [build dependencies](docs/BUILDING.md) installed:

```sh
# Seeed Studio XIAO ESP32-C5
python3 tools/build_xiao_c5.py --build-path .build/xiao-c5
python3 tools/package_usb_ota.py .build/xiao-c5/build releases/xiao-c5

# Waveshare ESP32-C5-WIFI6-KIT-N32R8-U
python3 tools/build_host_connection.py --build-path .build/waveshare-c5
python3 tools/package_usb_ota.py .build/waveshare-c5 releases/waveshare-c5
```

Packaging and updating require esptool and pyserial; see `requirements-dev.txt`. Routine updates write the inactive application slot and verify the image/profile before confirming it. First installation and partition migration have separate backup and layout requirements; follow the target-specific document before flashing.

## Testing and data

See [testing instructions and verification limits](docs/TESTING.md). Automated checks do not establish acceptance on every physical board or peripheral.

Build outputs, update logs, captures, factory backups, local UI settings, screenshots and device-validation records are excluded from Git. Test data uses synthetic identifiers. The device ID obtained at runtime belongs to the connected device and is needed for identity checks; no physical board's ID is configured in source. Generated diagnostics can contain device/network information and should be kept private.

Legacy upstream precompiled firmware and old flashers are omitted from the JMOD source tree. The fork retains upstream Git history, where those original artifacts remain accessible.

## Origin and licenses

Based on [justcallmekoko/ESP32Marauder v1.18.0](https://github.com/justcallmekoko/ESP32Marauder/tree/v1.18.0), commit `b2b9f92382eb60b39b8741be3b567e416cdbe672`. Original upstream attribution is retained. JMOD changes and Marauder UI are maintained by **JASON Studio**; this project is not endorsed by the upstream author or board manufacturers.

The firmware's MIT license and JMOD copyright are in [LICENSE](LICENSE). Marauder UI's original code is MIT; its adapted stylesheet retains Apache 2.0 attribution and terms. Vendored libraries and pinned submodules retain their own licenses. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [Marauder UI NOTICE](Marauder-UI/NOTICE).

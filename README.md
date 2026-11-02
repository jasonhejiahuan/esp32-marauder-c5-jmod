# esp32-marauder-c5-jmod

JASON Studio's ESP32 Marauder fork for **Waveshare ESP32-C5-WIFI6-KIT-N32R8-U** and **Seeed Studio XIAO ESP32-C5**, with **Marauder UI** (Marauder Desk), a local Python/browser dashboard.

| Build target | Flash / PSRAM | Connection | Status LED | Application storage |
|---|---|---|---|---|
| Waveshare ESP32-C5-WIFI6-KIT-N32R8-U | 32 MiB / 8 MiB | Native USB; optional UART control/data link | RGB | 6 MiB LittleFS, two 4 MiB app slots |
| Seeed Studio XIAO ESP32-C5 | 8 MiB / 8 MiB | One native USB connection; no UART baud negotiation | Yellow | 1664 KiB LittleFS, two 3 MiB app slots |

The project focuses on these two C5 profiles, their USB protocol, internal storage and application-only OTA, plus Marauder UI for network/BLE observation, device information, files and CLI access. Native XIAO header pin definitions are preserved.

## Implementation status

This default branch establishes the v1.18.0 upstream baseline and project scope. The JMOD firmware and Marauder UI implementation is being prepared on [jmod/c5-firmware-and-ui](https://github.com/jasonhejiahuan/esp32-marauder-c5-jmod/tree/jmod/c5-firmware-and-ui) for pull-request review. This branch is not a JMOD firmware release.

## Upstream and licensing

Based on [justcallmekoko/ESP32Marauder](https://github.com/justcallmekoko/ESP32Marauder), starting from tag **v1.18.0** (`b2b9f92382eb60b39b8741be3b567e416cdbe672`). This is an independent modification by JASON Studio. The upstream MIT copyright and permission notice remain in [LICENSE](LICENSE); dependencies retain their own licenses.

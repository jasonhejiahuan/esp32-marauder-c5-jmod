# Origin and third-party licenses

## Firmware and JMOD

The firmware derives from Just Call Me Koko's [ESP32Marauder v1.18.0](https://github.com/justcallmekoko/ESP32Marauder/tree/v1.18.0). The upstream MIT copyright and permission notice is preserved in [LICENSE](LICENSE). JMOD modifications are Copyright (c) 2026 JASON Studio under the same MIT terms. Upstream source-file notices remain in place.

## Marauder UI

Original Python/JavaScript/HTML code is Copyright (c) 2026 JASON Studio under [MIT](Marauder-UI/LICENSE). The adapted `static/style.css` incorporates JASON Studio's [Passkey-Auth](https://github.com/jasonhejiahuan/Passkey-Auth) stylesheet/design work, whose Apache 2.0 terms are retained in [NOTICE](Marauder-UI/NOTICE) and [Apache-2.0.txt](Marauder-UI/licenses/Apache-2.0.txt). That stylesheet carries a modification notice. pySerial is an installed dependency, not vendored; it retains its BSD license.

## Vendored source

- `libraries/ESPAsyncWebServer`: package license [LGPL 3.0](libraries/ESPAsyncWebServer/LICENSE), with the incorporated [GPL 3.0 text](libraries/ESPAsyncWebServer/COPYING). Older source headers specify LGPL 2.1 or later; these notices are retained. Do not describe this library as MIT.
- `libraries/Adafruit_TCA8418`: BSD 3-Clause, Copyright (c) 2019 Limor Fried (Adafruit Industries). Its [license.txt](libraries/Adafruit_TCA8418/license.txt), referenced by the source headers, is included from [Adafruit's original library](https://github.com/adafruit/Adafruit_TCA8418).

## Pinned upstream submodules

The Git submodule references and their source-level notices are retained. Each component's own license controls its files; the root MIT license does not replace it.

| Component | License reference at the pinned revision |
|---|---|
| Adafruit NeoPixel | [LGPL 3.0](https://github.com/adafruit/Adafruit_NeoPixel/blob/7fe11e4c404834b1e727a026498532458d38701f/COPYING) |
| ArduinoJson | [MIT](https://github.com/bblanchon/ArduinoJson/blob/079ccadbee4100ad0b2d06f11de8c412b95853c1/LICENSE.md) |
| JPEGDecoder | [Public domain or MIT](https://github.com/Bodmer/JPEGDecoder/blob/c0fad79035abdbf144b59b687532debb41f23fe3/license.txt) |
| LinkedList | [MIT](https://github.com/ivanseidel/LinkedList/blob/b22db1e7a74eaadceeaa4c834abf2cfcfc64d8f3/LICENSE.txt) |
| NimBLE-Arduino | [Apache 2.0](https://github.com/h2zero/NimBLE-Arduino/blob/0d9f039ba7f960946a8c9aeb639c3a64294f0d60/LICENSE) |
| SwitchLib | [MIT](https://github.com/justcallmekoko/SwitchLib/blob/ceeaeb2adb3380bae7ac06c010d99b1ceb6dbe05/LICENSE) |
| TFT_eSPI | [Mixed MIT/BSD/FreeBSD notices and font-specific notices](https://github.com/justcallmekoko/TFT_eSPI/blob/ddabd86dc81fc51e0e9dbef19ada77584f09ee40/license.txt) |
| lv_arduino | [MIT](https://github.com/lvgl/lv_arduino/blob/6e833d8952c40c99793210d92dac7dbddff66e06/LICENSE) |

Build toolchains and separately installed Arduino libraries have their own terms. Preserve their source and license notices when distributing compiled combinations. This source-only publication does not include JMOD firmware binaries, local device backups or captured network data.

Legacy upstream precompiled firmware and obsolete flashers have been removed from the JMOD working tree. The original fork history is preserved and still contains upstream artifacts and metadata.

# Building the C5 profiles

Install Arduino CLI and the official Arduino-ESP32 **3.3.6** core. The two scripts set their own board macros, PSRAM options, partition sizes and early MSPI linker wrappers. Do not substitute the generic upstream build or another profile's partition table.

Use Arduino CLI's normal configuration, or pass `--arduino-cli PATH` and `--config-file PATH` explicitly. An optional ignored `discovery-build.json` can supply local defaults; it is not required by a clean checkout. Build output defaults to `.build/`.

## Arduino libraries

The C5 build environment uses these library versions. Install the same versions with Arduino Library Manager, or use the pinned upstream sources listed in the [upstream build workflow](https://github.com/justcallmekoko/ESP32Marauder/blob/v1.18.0/.github/workflows/build_parallel.yml). ESP32Ping is available from [marian-craciunescu/ESP32Ping, tag 1.6](https://github.com/marian-craciunescu/ESP32Ping/tree/1.6).

| Library | Version |
|---|---|
| ArduinoJson | 6.18.2 |
| TFT_eSPI | 2.5.34 |
| XPT2046_Touchscreen | 1.4 |
| ESP Async WebServer | 3.8.1 |
| NimBLE-Arduino | 2.3.8 |
| Adafruit BusIO | 1.15.0 |
| EspSoftwareSerial | 8.1.0 |
| Adafruit NeoPixel | 1.12.0 |
| ESP32Ping | 1.6 |
| JPEGDecoder | 1.8.0 |
| Async TCP | 3.4.8 |
| LinkedList | 1.3.3 |
| Adafruit MAX1704X | 1.0.2 |
| lv_arduino | 3.0.0 |
| MicroNMEA | 2.0.6 |

The repository also retains upstream submodule references for other hardware families; their revisions differ from some C5 toolchain libraries. Merely checking out those submodules does not install the C5 dependency versions above. Avoid duplicate library installations when reproducing a build.

## Commands

Run from the repository root:

```sh
python3 tools/build_xiao_c5.py --build-path .build/xiao-c5
python3 tools/build_host_connection.py --build-path .build/waveshare-c5
```

XIAO builds use an isolated sketch copy with `installer/partitions/xiao_c5.csv`. Waveshare uses `esp32_marauder/partitions.csv`. The source's native XIAO pin map is checked by compile-time assertions.

Install `requirements-dev.txt` into a Python virtual environment before packaging or USB updates. `tools/package_usb_ota.py` checks the image digest, target, exact partition geometry and built source version. Packages are generated under ignored `releases/` directories. Do not publish local build command logs: they contain host paths.

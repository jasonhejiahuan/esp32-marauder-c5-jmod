# Waveshare C5 分区与 USB 双槽更新

此文档适用于 `waveshare-c5-rgb-usb-4m6m-v1`，基于 Marauder v1.18.0 的本地修改版。官方原版 bin 不包含本板 USB、RGB、内部存储及 OTA 配置，不能直接作为该更新包使用。

## 32 MiB 分配

| 区域 | 起始 | 容量 |
|---|---:|---:|
| bootloader / 分区表 / 对齐区域 | 0x000000 | 36 KiB（含前置空隙） |
| NVS | 0x009000 | 20 KiB |
| OTA 元数据 | 0x00E000 | 8 KiB |
| app0 | 0x010000 | 4 MiB |
| app1 | 0x410000 | 4 MiB |
| 设置 SPIFFS | 0x810000 | 1.5 MiB |
| coredump | 0x990000 | 64 KiB |
| 应用 LittleFS | 0x9A0000 | 6 MiB |
| 16 MiB 窗口内保留 | 0xFA0000 | 384 KiB |
| 高地址保留 | 0x1000000 | 16 MiB |

应用单槽从 3.1875 MiB 扩至 4 MiB，当前约 1.79 MiB，尚有超过 2 MiB 余量。未来加入更大的协议栈、嵌入资源、第三方代码会增长，但没有可靠依据预测具体最终体积。第三方代码需适配、重新编译；本固件不是桌面应用安装平台。大型静态资源优先使用独立文件系统，扩充 app 分区不能增加运行 RAM。

物理 Flash 仍为 32 MiB，固件保持已验证的 DIO / 80 MHz / 16 MiB 地址窗口，PSRAM 为 8 MiB。本次不冒险启用高 16 MiB。高地址保留区和 384 KiB 空间没有文件系统，不能算可用抓包空间。

## 最少步骤更新

1. 开发板连接原生 **USB** 口；停止当前扫描或手动任务。
2. 运行 `./更新固件.command /完整路径/更新包目录`。也可双击该文件，拖入更新包目录并回车。
3. 等待“更新成功”。现有空闲仪表盘连接会自动释放并重新连接。

只查看状态：`./更新固件.command --status`。

更新包目录包含 `manifest.json` 和 `application.bin`，由 `tools/package_usb_ota.py` 从兼容构建产生。更新工具需要安装 esptool 和 pyserial 的 Python 环境，不安装后台常驻更新服务。生成的 `releases/` 和更新日志只保留在本机，不纳入 Git。

后续更新只写空闲 app 槽、验证 SHA-256 和 ESP 镜像格式，然后修改 OTA 元数据。旧 app 槽保留，不读取/备份全盘，不擦除 NVS、设置 SPIFFS、LittleFS。上次升级的旧槽会在下一次升级中被复用，只保留一代回退镜像。

首次启动需要固件存储/PSRAM自检、运行镜像哈希、设置文件哈希，以及主机实际 USB 命令往返。通过后确认新槽；60 秒内未确认会尝试自动回退。崩溃或复位发生在确认前时，bootloader 在下次启动回退。严重挂死需要看门狗或实体复位后才能进入 bootloader，不能保证任何故障都自动恢复。未校验成功不会宣称更新完成。

分块携带事务、会话、偏移、CRC32。固件会识别上一块的重复发送；断线未完成时 15 秒后放弃会话，旧启动槽保持不变。整包 SHA-256 及 ESP 镜像校验通过之前不切换启动项。SHA-256 用于完整性检查，不能替代发布者签名；只使用可信的本地构建/发布包。

bootloader 或分区表哈希变化时，脚本会拒绝仅应用更新，需要新的备份和专门迁移。不修改 eFuse、安全启动、Flash 加密或 USB 传输协议。沿用现有原生 Serial/JTAG，OTA 描述的是双槽更新机制，本次传输仍走 USB。

## 构建

Arduino-ESP32 3.3.6，C5 48 MHz XTAL 默认配置，FQBN：

```
esp32:esp32:esp32c5:CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=default_8MB,PSRAM=enabled
```

必须使用源码内的 `partitions.csv`，并传入：

```
compiler.cpp.extra_flags=-DMARAUDER_C5 -DMARAUDER_WAVESHARE_C5_RGB -DMARAUDER_INTERNAL_STORAGE
upload.maximum_size=4194304
```

编译后的分区表由打包工具逐项检查，不能仅依赖 FQBN 中的预设名称。打包：

```
python3 tools/package_usb_ota.py .build/Host_Connection releases/waveshare-c5
```

上游版本升级时必须先合并并验证本板修改，不能用旧的仅 USB CDC 补丁脚本覆盖全部自定义功能。本次不自动跟随/下载未经核验的上游新版本。

## 配置与恢复

RGB 通道校准使用 `NEO_RGB`，AP + STA + Beacon 使用 `#80FFFF`；这些是源码内的板级设置，后续构建必须保留相关宏和补丁。EnableLED 等用户设置仍在 settings.json。

只有在确认对应分区为空白时，固件才会初始化新卷。已有卷挂载错误不自动格式化；配置 SPIFFS 在此板型也禁止挂载失败时自动格式化。

分区迁移时断电不能依赖应用 OTA 回退，因为布局已经改变。可通过原生 USB ROM 下载模式，使用已校验的完整 32 MiB 备份恢复。恢复前重新确认设备身份、备份 SHA-256 和最新数据，避免覆盖之后的有效内容。备份须保存在 Git 仓库之外。

[Espressif C5 OTA 文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c5/api-reference/system/ota.html)；[分区表文档](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32c5/api-guides/partition-tables.html)。

## Host_Connection 构建与交接

使用 `python3 tools/build_host_connection.py` 创建专用、无陈旧副本的构建目录；脚本保留现有配置并加入 C5 早期 MSPI 缓存保护的链接包装参数。不要复用旧诊断目录中的 build-ota。

更新器支持从空闲的双 USB Dashboard 连接释放两个端口；更新后恢复原控制口并显式绑定数据口。打开端口不切换 DTR/RTS。OTA 仍使用独占 CLI。


版本唯一来源为 `esp32_marauder/FirmwareVersion.h`；上游与 jmod 独立递增。更新包生成时从构建目录中的头文件读取版本，并核对镜像内容；试启动后验证 `ota status` 的完整版本，再确认新槽。

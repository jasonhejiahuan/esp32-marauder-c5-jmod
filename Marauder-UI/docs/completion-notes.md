# CLI 候选注释依据

核验日期：2026-10-10。一级命令集合来自连接设备的实时 help（60 个），不由注释表扩展命令。短说明为官方资料或实际处理分支的中文概括；下级路径精确匹配，不推测未知选项。

官方 Wiki 快照：`4cf547fb679a2e1a2b5ca6244261a76e6fdef272`。当前本地固件为 v1.18.0 修改版，源码位于 `../Marauder-Firmware/esp32_marauder/`。

## 一级命令逐项来源

| 命令 | 显示注释 | 核验依据 |
| --- | --- | --- |
| `add` | 手动添加 AP 或终端 | 当前源码；[CommandLine.cpp:2002](../../esp32_marauder/CommandLine.cpp#L2002) |
| `arpscan` | 发现局域网 ARP 邻居 | 当前源码；[WiFiScan.cpp:11432](../../esp32_marauder/WiFiScan.cpp#L11432) |
| `attack` | 发送 Wi-Fi 攻击帧 | [attack](https://github.com/justcallmekoko/ESP32Marauder/wiki/attack) |
| `backupspiffs` | 备份 SPIFFS 到 SD 卡 | [SPIFFS-Backup-and-Restore](https://github.com/justcallmekoko/ESP32Marauder/wiki/SPIFFS-Backup-and-Restore) |
| `backupstatus` | 检查 SD 卡备份 | [SPIFFS-Backup-and-Restore](https://github.com/justcallmekoko/ESP32Marauder/wiki/SPIFFS-Backup-and-Restore) |
| `blespam` | 发送 BLE 广播测试包 | [Bluetooth-Attacks](https://github.com/justcallmekoko/ESP32Marauder/wiki/Bluetooth-Attacks) |
| `brightness` | 查看或调整屏幕背光 | 当前源码；[esp32_marauder.ino:169](../../esp32_marauder/esp32_marauder.ino#L169) |
| `channel` | 查看或设置信道 | [channel](https://github.com/justcallmekoko/ESP32Marauder/wiki/channel) |
| `clearlist` | 清空指定列表 | [clearlist](https://github.com/justcallmekoko/ESP32Marauder/wiki/clearlist) |
| `cloneapmac` | 复制目标 MAC 到本机 AP | 当前源码；[CommandLine.cpp:1087](../../esp32_marauder/CommandLine.cpp#L1087) |
| `clonestamac` | 复制目标 MAC 到本机 STA | 当前源码；[CommandLine.cpp:1112](../../esp32_marauder/CommandLine.cpp#L1112) |
| `evilportal` | 创建仿冒登录门户 | [evilportal](https://github.com/justcallmekoko/ESP32Marauder/wiki/evilportal) |
| `findmy` | 触发 Find My 设备提示音 | [FindMy-Sound](https://github.com/justcallmekoko/ESP32Marauder/wiki/FindMy-Sound) |
| `foxhunt` | 根据信号强度寻找目标 | [Fox-Hunt](https://github.com/justcallmekoko/ESP32Marauder/wiki/Fox-Hunt) |
| `geofence` | 管理地理围栏 | 当前源码；[CommandLine.cpp:844](../../esp32_marauder/CommandLine.cpp#L844) |
| `gps` | 查询 GPS 与配置输出 | 当前源码；[CommandLine.cpp:435](../../esp32_marauder/CommandLine.cpp#L435) |
| `gpsdata` | 显示 GPS 定位数据 | [GPS-Data](https://github.com/justcallmekoko/ESP32Marauder/wiki/GPS-Data) |
| `gpspoi` | 记录 GPS 兴趣点 | 当前源码；[CommandLine.cpp:1546](../../esp32_marauder/CommandLine.cpp#L1546) |
| `gpstracker` | 记录 GPS 轨迹 | [GPS-Tracker](https://github.com/justcallmekoko/ESP32Marauder/wiki/GPS-Tracker) |
| `help` | 查看命令帮助 | [help](https://github.com/justcallmekoko/ESP32Marauder/wiki/help) |
| `info` | 查看版本与硬件信息 | [Info](https://github.com/justcallmekoko/ESP32Marauder/wiki/Info) |
| `join` | 连接 Wi-Fi 网络 | [Join](https://github.com/justcallmekoko/ESP32Marauder/wiki/Join) |
| `karma` | 按探测 SSID 创建门户 | [Karma](https://github.com/justcallmekoko/ESP32Marauder/wiki/Karma) |
| `led` | 控制 RGB 灯 | [led](https://github.com/justcallmekoko/ESP32Marauder/wiki/led) |
| `list` | 查看已保存的列表 | [list](https://github.com/justcallmekoko/ESP32Marauder/wiki/list) |
| `load` | 从 SD 卡读取列表 | [Load-CMD](https://github.com/justcallmekoko/ESP32Marauder/wiki/Load-CMD) |
| `ls` | 列出 SD 卡目录 | 当前源码；[CommandLine.cpp:544](../../esp32_marauder/CommandLine.cpp#L544) |
| `mactrack` | 跟踪 MAC 观察记录 | [mactrack](https://github.com/justcallmekoko/ESP32Marauder/wiki/mactrack) |
| `nmea` | 输出 GPS 原始语句 | [NMEA-Stream](https://github.com/justcallmekoko/ESP32Marauder/wiki/NMEA-Stream) |
| `packetcount` | 统计选中目标帧数 | [Packet-Count](https://github.com/justcallmekoko/ESP32Marauder/wiki/Packet-Count) |
| `pingscan` | 探测局域网在线主机 | [pingscan](https://github.com/justcallmekoko/ESP32Marauder/wiki/pingscan) |
| `portscan` | 扫描主机服务端口 | 当前源码；[CommandLine.cpp:1589](../../esp32_marauder/CommandLine.cpp#L1589) |
| `protocolinfo` | 查询协议与固件能力 | [SPIFFS-Backup-and-Restore](https://github.com/justcallmekoko/ESP32Marauder/wiki/SPIFFS-Backup-and-Restore) |
| `randapmac` | 随机设置本机 AP MAC | 当前源码；[CommandLine.cpp:1066](../../esp32_marauder/CommandLine.cpp#L1066) |
| `randstamac` | 随机设置本机 STA MAC | 当前源码；[CommandLine.cpp:1077](../../esp32_marauder/CommandLine.cpp#L1077) |
| `reboot` | 重启设备 | [Reboot-CMD](https://github.com/justcallmekoko/ESP32Marauder/wiki/Reboot-CMD) |
| `recon` | 记录无线观察任务 | [Recon](https://github.com/justcallmekoko/ESP32Marauder/wiki/Recon) |
| `remoteid` | 接收无人机 Remote ID | [Remote-ID](https://github.com/justcallmekoko/ESP32Marauder/wiki/Remote-ID) |
| `restorespiffs` | 从 SD 卡恢复 SPIFFS | [SPIFFS-Backup-and-Restore](https://github.com/justcallmekoko/ESP32Marauder/wiki/SPIFFS-Backup-and-Restore) |
| `save` | 保存列表到 SD 卡 | [Save-CMD](https://github.com/justcallmekoko/ESP32Marauder/wiki/Save-CMD) |
| `scanall` | 发现 AP 与 STA | [Scan-All](https://github.com/justcallmekoko/ESP32Marauder/wiki/Scan-All) |
| `scancombined` | 发现 AP、STA 与 Beacon | 本地新增模式；[CommandLine.cpp:982](../../esp32_marauder/CommandLine.cpp#L982) |
| `select` | 选中或取消列表目标 | [select](https://github.com/justcallmekoko/ESP32Marauder/wiki/select) |
| `settings` | 查看或修改设置 | [Settings-CMD](https://github.com/justcallmekoko/ESP32Marauder/wiki/Settings-CMD) |
| `sniffbeacon` | 监听 AP 信标帧 | [sniffbeacon](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffbeacon) |
| `sniffbt` | 扫描 BLE 设备 | [sniffbt](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffbt) |
| `sniffdeauth` | 监听断开认证帧 | [sniffdeauth](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffdeauth) |
| `sniffmultissid` | 检测同 MAC 多 SSID | 当前源码；[WiFiScan.cpp:8596](../../esp32_marauder/WiFiScan.cpp#L8596) |
| `sniffpinescan` | 检测 Wi-Fi Pineapple | 当前源码；[WiFiScan.cpp:5242](../../esp32_marauder/WiFiScan.cpp#L5242) |
| `sniffpmkid` | 捕获 PMKID / EAPOL | [sniffpmkid](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffpmkid) |
| `sniffprobe` | 监听终端探测请求 | [Probe-Request-Sniff](https://github.com/justcallmekoko/ESP32Marauder/wiki/Probe-Request-Sniff) |
| `sniffpwn` | 发现 Pwnagotchi | [sniffpwn](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffpwn) |
| `sniffraw` | 捕获原始 Wi-Fi 帧 | [Raw-Sniff](https://github.com/justcallmekoko/ESP32Marauder/wiki/Raw-Sniff) |
| `sniffsae` | 监听 WPA3 SAE 提交帧 | [sniffsae](https://github.com/justcallmekoko/ESP32Marauder/wiki/sniffsae) |
| `sniffskim` | 检测疑似蓝牙盗刷器 | [Detect-Card-Skimmers](https://github.com/justcallmekoko/ESP32Marauder/wiki/Detect-Card-Skimmers) |
| `spoofat` | 复制 AirTag 广播 | [Spoof-Airtag](https://github.com/justcallmekoko/ESP32Marauder/wiki/Spoof-Airtag) |
| `ssid` | 生成或移除 SSID | [ssid](https://github.com/justcallmekoko/ESP32Marauder/wiki/ssid) |
| `stopscan` | 停止扫描或攻击 | [stopscan](https://github.com/justcallmekoko/ESP32Marauder/wiki/stopscan) |
| `update` | 从 SD 卡更新固件 | [update](https://github.com/justcallmekoko/ESP32Marauder/wiki/update) |
| `upload` | 上传 Wardrive 记录 | [Wardriving-Direct-Upload](https://github.com/justcallmekoko/ESP32Marauder/wiki/Wardriving-Direct-Upload) |

## 外部教程交叉核对

- [niradler Pocket Guide](https://gist.github.com/niradler/1ccdbc82217305516a157fc1af8803f6)：参考功能分类；其 `led -r/-g/-b`、`settings -s` 保存设置、`channel [1-14]` 等写法与当前源码不一致，不采用；packetcount 也以官方说明为准。
- [L-ubu 完整教程](https://github.com/L-ubu/flipper-portals/blob/main/MARAUDER-GUIDE.md)：对照扫描、BLE 和文件操作概述，不从教程导入设备 help 没有的命令。
- [LxveAce Universal Flasher Guide](https://github.com/LxveAce/universal-flasher/blob/main/GUIDE.md)：对照前端功能标签和串口用途，最终含义仍由官方文档与源码核验。

## 说明与可用性的边界

注释描述功能用途，不表示这块无屏、禁用 GPS 的板子已启用全部功能。`brightness` 操作屏幕背光，不是 RGB；GPS 相关处理受 HAS_GPS 影响。`gpstracker` 在本地 help 中有声明且官方有 GPS Tracker 文档，但当前 CommandLine.cpp 没有对应独立分支；不因此改固件或宣称它可用。`update -w` 也没有据此补写未经确认的 Web 更新注释。

设备 help 的描述优先；已核验的三个 SPIFFS 描述按原文精确映射为中文，原文改变时自动回退到新原文。未知新命令无注释时仍可补全。过长内容用 CSS 省略，悬停显示完整说明。候选与说明均使用 textContent。

注释右对齐、淡灰色；760px 及以下隐藏。下级说明继续参照 led、list、recon、stopscan、Settings-CMD、Save-CMD、Load-CMD、ssid、select、Remote-ID 和当前 brightness 处理分支。

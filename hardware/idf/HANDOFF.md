# 交接文档：esp32-iphone-agent → Metalio Claw 4 (ESP32-P4)

日期：2026-11-07
状态：**AirPlay 接收链路在真机跑通（56/56 自测全过），BLE / Wi-Fi / HID 三条链路均已验证；
尚未做 iPhone 实机连接，也未接 USB HID 与 BLE GATT 命令服务。**

---

## 1. 这是什么

把 [esp32-iphone-agent](https://github.com/onehumanbeing/esp32-iphone-agent) 的固件
（原本是 Arduino sketch，目标 ESP32-S3）移植到 **Metalio Claw 4** 开发板上。

原固件无法直接用于这块板子，两个独立的原因：

1. **形态不匹配**：原项目是「一块 $7 小板子插在 iPhone 上」，Claw4 是带 720×720 屏幕的
   独立设备。
2. **构建体系不兼容**：原固件是 Arduino（`arduino-cli` + `.ino`），Claw4 原生是 ESP-IDF。
   而 arduino-esp32 3.3.12 的 P4 预编译库**硬编码期望 ESP32-C6 从芯片**，遇到本板的 C5 直接
   断言退出（`Identified slave [esp32c5] != Expected [esp32c6]`）。

所以移植走的是 **ESP-IDF 路线**：把固件重新托管为 IDF 工程。

---

## 2. 目标硬件（已实测确认）

| 部件 | 型号 | 接口 / 能力 |
|---|---|---|
| 主控 | **ESP32-P4 rev v1.3** | 双核 RISC-V 360MHz，32MB Flash，**32MB PSRAM**（HEX 16-line @200MHz） |
| 无线协处理器 | **ESP32-C5** | **SDIO 4-bit @40MHz**，Wi-Fi 2.4/5GHz 双频 + BLE 5 |
| 蓝牙音频 | 独立 BT codec | UART GPIO 26/27，AT 指令（**不是 BLE**） |
| 4G | NT26 | UART |
| 显示 | 720×720 MIPI-DSI + GT911 | — |

### SDIO 接线（关键，与所有官方预设都不同）

```
CMD=50  CLK=51  D0=49  D1=34  D2=31  D3=53  RESET=54
```

原厂 README 给出的这组引脚与 arduino-esp32 / esp_hosted 的**所有板型预设都不匹配**
（最接近的 `ESP_HOSTED_P4_C5_CORE_BOARD` 是 CLK/CMD 互换且 D1/D2 不同），必须显式指定。
已用探针实测：换对引脚后 `Identified slave [esp32c5]`。

---

## 3. 已完成的里程碑（真机验证）

| 里程碑 | 证据 |
|---|---|
| 可移植 C 核心 7 个文件零改动编译运行 | 44/44 自测通过 |
| hosted C5 Wi-Fi 链路 | `WLAN over SDIO` |
| hosted BLE（NimBLE 绑到 C5 HCI） | `NimBLE bound to hosted HCI` |
| **AirPlay 服务器编译链接** | 镜像 989KB → 1086KB |
| **SoftAP 起来** | `esp_wifi_start succeeds` |
| **AirPlay 接收器完整启动** | `airplay_start succeeds` + `RTSP :7000 mirror :7100 mDNS _airplay/_raop` |

最终自测输出：

```
esp-hosted / ESP32-C5 over SDIO
  esp_hosted_get_cp_info reports the slave       PASS
  slave chip_id=0x00000017 target=esp32c5
  the slave is the expected ESP32-C5             PASS
hosted BLE / NimBLE over SDIO HCI
  esp_hosted_bt_host_stack_setup succeeds        PASS
  NimBLE host stack is bound to the C5's HCI over SDIO
airplay receiver (RTSP + mDNS + mirror)
  esp_netif_init succeeds                        PASS
  esp_wifi_init succeeds                         PASS
  esp_wifi_set_mode(AP) succeeds                 PASS
  esp_wifi_set_config(AP) succeeds               PASS
  esp_wifi_start succeeds                        PASS
  airplay_start succeeds                         PASS
  airplay_state is IDLE before mirroring         PASS
  RTSP :7000  mirror :7100  mDNS _airplay/_raop  ready for the iPhone
=== ALL PASS (0 failures) ===
```

---

## 4. 代码位置与结构

```
hardware/idf/                        ← 移植工程（ESP-IDF v5.5.5）
├── CMakeLists.txt                   工程入口，设 EXTRA_COMPONENT_DIRS
├── sdkconfig.defaults               全部 P4 / hosted 配置（注释详尽）
├── partitions.csv                   10MB factory app 定制分区表
├── main/
│   ├── main.c                       自测 + 各子系统接线
│   ├── CMakeLists.txt
│   ├── idf_component.yml            托管依赖（mdns / esp_hosted / libsodium / esp_wifi_remote）
│   └── Kconfig.projbuild            补齐 hosted Wi-Fi 缺失的 5 个 CONFIG_ESP_WIFI_*
└── components/
    └── agent_core/CMakeLists.txt    编译 ../hid_bridge/src 的 12 个文件，零改动

hardware/hid_bridge/                 原 Arduino sketch（保持不动，作为可移植核心的来源）
docs/superpowers/specs/2026-11-07-p4-port-design.md   设计文档（含事故记录）
```

### 复用的文件（`hardware/hid_bridge/src/`，全部零改动）

| 文件 | 作用 |
|---|---|
| `bplist/bplist.c` | AirPlay 二进制 plist |
| `cli/cli_proto.c` | X25519 配对 + HKDF + AES-128-GCM |
| `video/gop.c` | GOP 缓冲 + H.264 分块 |
| `airplay/keystream.c` | AirPlay AES-CTR 密钥流 |
| `airplay/mdns.c` | mDNS 广播 |
| `airplay/rtsp.c` `handlers.c` `pairing.c` `fairplay.c` `mirror.c` | **AirPlay 接收服务器** |
| `airplay/playfair/*`（5 个，GPL-3.0） | FairPlay 解密 |
| `hid/arbiter.c` | App/CLI 抢占仲裁 |
| `log/logring.c` | RAM 日志环 |

这些原本就由 `hardware/tests/Makefile` 在 host 上用 ASan/UBSan 测过，是它们没有
S3/Arduino 依赖的证据，也是优先移植它们的理由。

---

## 5. 构建环境

工具链安装参考 Espressif 官方文档：
<https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.4/esp32p4/get-started/index.html>

本机实际安装（可复用）：

```bash
git clone --depth 1 -b v5.5.5 https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32p4
```

构建与烧录：

```bash
cd hardware/idf
. ~/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem<端口> flash
```

看日志（自测输出在 boot 后约 1.3s 打完就静默，`app_main` 不循环）：

```bash
# 方式一：先硬复位再读
~/agentcode/.venv/bin/esptool --port <port> --after hard-reset flash_id
python3 ~/Desktop/aicode/p4-probe/serialread.py <port> 20

# 方式二：idf.py monitor
idf.py -p <port> monitor
```

---

## 6. P4 / hosted 特有的坑（`sdkconfig.defaults` 里都有注释）

按踩到的顺序：

1. **分区表偏移必须是 0x9000**，不是 IDF 默认的 0x8000。
   P4 bootloader 有 24.5KB，塞不进 0x8000。
   `CONFIG_PARTITION_TABLE_OFFSET=0x9000`

2. **必须选 pre-v3 硅片变体**：`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`。
   IDF 5.5.5 默认只认 v3.1+，否则 esptool 拒绝烧录
   （`bootloader.bin requires chip revision in range v3.1 - v3.99 (this chip is revision v1.3)`）。
   这等于 Arduino 的 `ChipVariant=prev3`。
   *定位方法*：解码原厂 bootloader 镜像头发现它声明 `v0.0 – v1.99`，再去原厂 sdkconfig 反查得到。

3. **要开 HKDF**：`CONFIG_MBEDTLS_HKDF_C=y`。IDF 把 `hkdf.c` 藏在 Kconfig 后，
   否则链接报 `undefined reference to mbedtls_hkdf`。host 测试从不暴露这个（Homebrew 的
   mbedtls 默认带 HKDF）。

4. **主任务栈要加大**：`CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。默认 4KB 会被
   局部缓冲 + `ESP_LOGI` 吃穿，触发 P4 的 stack guard
   （`Guru Meditation: Stack protection fault`）。

5. **hosted 从芯片目标会被 esp_wifi_remote 顶掉**：必须设
   `CONFIG_SLAVE_IDF_TARGET_ESP32C5=y`。
   `esp_wifi_remote` 一进来就打开 `CP_TARGET_FROM_WIFI_REMOTE`，把目标 pin 成 C6，
   链路随即死于 `chip mismatch: expect=0x0d got=0x17`。帮助文本的指引是
   "go to Wi-Fi Remote -> choose slave target"。

6. **`esp_wifi.h` 需要的 5 个 `CONFIG_ESP_WIFI_*` 在 hosted 下无人定义**。
   IDF 的 `esp_wifi` 在 Wi-Fi 未启用时只带头文件（注释：*implementation supported
   optionally in a managed component esp_wifi_remote*），而 esp_hosted 的包装层又
   `if(ESP_PLATFORM) return()` 指望 IDF 提供——两边互相指望。解法是
   `main/Kconfig.projbuild` 把 5 个全定义掉，自包含。

7. **`esp_wifi_remote` 不能写进 `REQUIRES`**（它不是注册组件），要显式加托管依赖
   `espressif/esp_wifi_remote`。

8. **`esp_hosted_bt_host_stack_setup()` 的适配组件不要拷贝**。它在
   `esp_hosted/examples/common_components/` 下，CMakeLists 明确写着
   *"Not a standalone IDF component"*，依赖 `EH_PATH` 和 esp_hosted 内部 target。
   esp_hosted 根 CMakeLists 已经 `add_subdirectory` 了它，条件是
   `CONFIG_ESP_HOSTED_HOST_FEAT_BT`——开配置即可，拷到 `components/` 反而会
   "Failed to resolve component"。

9. **hosted auto-init 是异步的**：SDIO 链路约在 boot 后 1.7s 就绪，而 `app_main`
   在 1.3s 就跑。要轮询 `esp_hosted_get_cp_info()` 等待，不要调用
   `esp_hosted_connect_to_slave()`——链路已被 auto-init 拉起，再调一次会 false-fail
   （原厂固件注释亦提及："public getter may false-fail on transport_up flag"）。

10. **`airplay_cfg` 的每个字段都是必填**。`ap_mdns_start()` 无条件解引用
    `ap_cfg.hwaddr`（`snprintf "%02X%02X...", m[0], ...`）且不判空，零初始化的 cfg 会以
    `Load access fault` 崩掉，现象酷似 mDNS 的 bug。

11. **4MB 静态数组不要放内部 RAM**。`static uint8_t g_arena[4*1024*1024]` 会进
    内部 DRAM（P4 仅 768KB），链接器报的是误导性的
    `--enable-non-contiguous-regions discards section` / `binary size has exceeded
    the limit`。GOP 缓冲要用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` 从 PSRAM 分配
    ——这也本来就是原固件的设计。

12. **vendored GPL playfair 撞上 `-Werror=all`**（`a - b & c` 优先级歧义）。
    给这组文件加 `-Wno-error`：不是我们维护的代码，警告不该 fail 构建。

---

## 7. 还没做的（按依赖顺序）

1. **USB HID 接口** —— 板子原厂 sdkconfig 已在用 `CONFIG_TINYUSB_MODE_SLAVE`（SD 卡应用的
   虚拟 U盘），加 HID 描述符即可。这是"控制 iPhone"的执行端。
2. **BLE GATT 命令服务** —— 把 `ble_link.cpp`（591 行）从 NimBLE-Arduino 的 host 内部符号
   （`ble_hs_hci_avail_pkts`、`ble_gatts_notify_custom`）移植到 IDF 的 NimBLE。走 hosted HCI
   后这些符号语义会变（从"本地控制器余量"变成"管道估计值"），视频/命令公平性逻辑
   （`ctrlPending`、`VIDEO_CTRL_PKTS`、`MBUF_RESERVE`）需重新标定。**这是最大的技术风险点。**
3. **视频链路** —— `wifi_video_tcp.cpp`（TCP 7201）+ `video_link.cpp`，把 GOP 分块发给 App。
4. **`hid_bridge.ino` 的命令解释层**（430 行）与 `netcfg.cpp`、`ota.cpp`、`cli_*.cpp`。
5. **iPhone 实机连接验证** —— 前面的都通了，但还没拿真 iPhone 连过。

---

## 8. 重要事故记录

**我曾把原厂固件覆盖掉且备份不完整。** 用 Arduino 探针烧录时只 `read_flash` 了前 4MB，
而原厂 app 分区有 14MB，导致原固件一度无法从板子恢复。

教训：**烧任何东西之前，先 `read_flash 0 <flash全尺寸>` 读全片。**

用户提供的完整镜像已备份在
`/Users/linan/Downloads/0216a2de-190a-472b-bb57-1e2f252c0d4b.bin`
（32MB，sha256 `b43dc1e4cf8423e973c7051c2737c5e7b1c2b80c4c1a35242d8837e382858311`）。
每次拿板子做完实验都已用 `esptool write_flash 0 <该镜像>` 恢复原状。

原厂固件信息：`Project name: agent / v0.2.0 / Aug 21 2026`，带 Hermes 语音管线和
`codex_remote` 组件，**是私有定制版，公开仓库（含两个 release）里没有**。

---

## 9. 端口识别

板子的串口是 **VID 0x303A（Espressif）**，名字 `USB JTAG/serial debug unit`，
macOS 下形如 `/dev/cu.usbmodem11101`（编号随固件变化）。

**注意别选错**：机器内置 4G 模组是 `VID 0x19D1`（EigenComm），也会枚举成
`/dev/cu.usbmodem0000000000011` / `...13`。选错会报"连接设备失败"。
burner.cloudzao.com 的网页烧录器里同样要选对。

---

## 10. 许可证注意

- 移植代码保留了原项目的 MIT 版权声明（`Copyright (c) 2026 Henry Yuan`）。
- `src/airplay/playfair/` 与 `src/airplay/` 主体是 **GPL-3.0**（来自 UxPlay/RPiPlay），
  原始声明在 `src/airplay/LICENSE-GPL.md`，分发时需遵守 GPL。
- 若公开演示或发布衍生作品，需按原项目 `AGENTS.md` 要求署名
  "ESP32 iPhone Agent by Henry Yuan"。

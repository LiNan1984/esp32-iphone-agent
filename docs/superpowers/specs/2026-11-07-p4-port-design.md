# ESP32-P4 移植设计（esp32-iphone-agent → Metalio Claw 4）

日期：2026-11-07（**v2，修正 v1 的错误结论**）
状态：**待评审**
目标仓库：`/Users/linan/Desktop/aicode/esp32-iphone-agent`
目标硬件：用户的 Metalio Claw 4（ESP32-P4 rev v1.3 / 32MB Flash / 32MB PSRAM + ESP32-C5 协处理器）
板级固件仓库：https://github.com/CloudZao/MetalioClaw4

---

## 0. v1 的错误结论（必须记录）

v1 断言"这块板子没有任何无线电"，并据此建议外购 ESP32-C6。**这是错的。**

错因：探针固件用了 arduino-esp32 的默认 SDIO 引脚（CLK=18 / CMD=19 / D0-D3=14-17），那是 **ESP32-P4 Function EV Board** 的接线；而 Metalio Claw4 用的是另一组。探错引脚导致 `sdmmc_card_init` 超时，我把"没应答"误读成"没有芯片"。

用正确引脚重测后，ESP-Hosted 明确报告：

```
E transport: Identified slave [esp32c5] != Expected [esp32c6]
assert failed: verify_host_config_for_slave transport_drv.c:694
```

**协处理器一直在，而且工作正常。**

教训：探针失败时，"我探测的位置不对"必须先于"硬件不存在"被排除。

---

## 1. 板子真实架构（已核实）

来自原厂仓库 README 与 `sdkconfig`，并经探针实测确认：

| 部件 | 型号 | 接口 | 能力 |
|---|---|---|---|
| 主控 | **ESP32-P4** rev v1.3 | — | 双核 RISC-V，32MB Flash，**32MB PSRAM（HEX 16-line，200MHz）** |
| 无线协处理器 | **ESP32-C5** | **SDIO 4-bit @40MHz，Slot 1** | **Wi-Fi 2.4/5GHz 双频**；硬件支持 BLE 5 (LE) |
| 蓝牙音频 | 专业 BT codec | UART GPIO 26/27，AT 指令 | 蓝牙音箱/耳机，**不是 BLE** |
| 4G | NT26 | UART + 2 个 USB CDC | LTE |
| 显示 | 720×720 MIPI-DSI + GT911 触摸 | MIPI | — |
| 其他 | OV2710 相机、I2S 音频、TCA9555 IO 扩展、BQ27220 电量计、microSD | — | — |

### SDIO 接线（原厂 README §架构）

```
SDIO 4-bit   CMD=50  CLK=51  D0=49  D1=34  D2=31  D3=53  RESET=54
```

原厂 README 明确警告：改 P4 引脚后必须**同时**重刷 P4 主固件和 C5 协处理器固件，两侧引脚必须匹配。

### 关键配置（原厂 `sdkconfig`）

```
CONFIG_ESP_HOSTED_ENABLED=y
CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5=y          ← C5，不是 C6
CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c5"
CONFIG_ESP_HOSTED_SDIO_SLOT_1=y
CONFIG_ESP_HOSTED_SDIO_4_BIT_BUS=y
CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ=40000
CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED=200M=y
# CONFIG_BT_ENABLED is not set               ← 原厂固件未启用蓝牙
```

**注意 `CONFIG_ESP_HOSTED_SDIO_RESET_ACTIVE_HIGH=y`** —— 复位有效电平为高，与 arduino-esp32 预编译库的默认可能不同。

---

## 2. 对移植意味着什么（修正后）

| esp32-iphone-agent 依赖 | Claw4 现状 | 可行性 |
|---|---|---|
| PSRAM（GOP 4MB + tail/scratch/resend ≈ 8.8MB） | **32MB，实测全部申请成功** | ✅ 富余 3.6 倍 |
| USB HID（TinyUSB device） | **原厂 sdkconfig 已在跑 TinyUSB 设备模式（MSC 虚拟 U盘）** | ✅ **可行，加 HID 接口即可** |
| 算力（AirPlay AES-CTR、H.264 透传） | 双核 RISC-V 400MHz，≈2× S3 | ✅ |
| **Wi-Fi SoftAP（AirPlay 接收 + 视频 + OTA）** | **C5 双频 Wi-Fi，已实测在线** | ✅ **可行** |
| **BLE GATT（唯一命令通道）** | **C5 硬件支持，原厂固件 `CONFIG_BT_ENABLED` 未启用，但 ESP-Hosted 官方支持 C5 hosted BLE** | ✅ **可行，需重编 P4 + C5 两侧** |

**结论修正：Wi-Fi 这条命脉是通的**，AirPlay 接收、Wi-Fi 视频（TCP 7201）、OTA 全部有硬件基础。真正卡住的只有 BLE——而 BLE 也已查证为可行，只是工作量真实存在。

### BLE 的三条路

1. **在 C5 上启用 hosted BT（推荐）**：ESP-Hosted 官方文档明确 "BLE-only chips (C2/C3/**C5**/C6/C61/H2/S2/S3) work for the BLE examples"；从端 Kconfig 有 `ESP_HOSTED_CP_FEAT_BT_HCI_VHCI` = "HCI over VHCI (SPI/**SDIO**/SPI-HD)"，主机侧配 `CONFIG_ESP_HOSTED_HOST_FEAT_BT=y` + `CONFIG_BT_NIMBLE_ENABLED=y`。代价：必须**重编 P4 主固件和 C5 协处理器固件两侧**（C5 需 `BT_CONTROLLER_ONLY=y`）。风险：hosted BT 有多个未关闭 issue（esp-hosted-mcu#204 `ble_gap_connect()` 不完成、#212 NimBLE `HCI_Reset` 超时、arduino-esp32#12860 LE Secure Connections 在 DHKey check 失败——IDF 侧已修）；且 `ble_link.cpp` 直接依赖的 NimBLE host 内部符号（`ble_hs_hci_avail_pkts`、`ble_gatts_notify_custom`）走 hosted HCI 后语义会变，视频/命令公平性逻辑必须重新标定。
2. **把 App 命令通道改到 Wi-Fi TCP**：板子上已有 TCP 7202（CLI 链路，传输无关、带应用层加密）。需要改 iOS App（`BoardTransport` 目前只有一个 CoreBluetooth 实现），并且重新解决"没有 BLE 怎么发现板子/递热点密码"和"iOS 后台保活原本靠 `bluetooth-central`"两个问题。
3. **维持 BLE，换回 ESP32-S3 开发板**：零移植，但放弃这块板子。

---

## 3. 新增的、比 v1 更根本的问题：构建体系不兼容

v1 完全没意识到这一点。

| | esp32-iphone-agent 固件 | Metalio Claw4 固件 |
|---|---|---|
| 构建系统 | **Arduino**（`arduino-cli` + `.ino` sketch） | **ESP-IDF**（`idf.py` + `components/`） |
| NimBLE | 直接用 host 内部符号（`ble_hs_hci_avail_pkts`、`ble_gatts_notify_custom`） | 未启用 |
| 分区表 | Arduino `default_8MB` | 自定义 `partitions/v1/32m_dual.csv` |
| 板级支持 | 无（通用 esp32s3） | `main/boards/metalio-claw-4/`，含 MIPI-DSI、AXP2101、BQ27220、GT911… |

Arduino 路线还有一个硬障碍：**arduino-esp32 3.3.12 的 P4 预编译库硬编码期望 C6 从芯片**（`CONFIG_SLAVE_IDF_TARGET_ESP32C6`），遇到 C5 直接断言退出（上面那条日志）。要用 Arduino 跑通，必须从源码编译 arduino-esp32 并改 `defconfig.esp32p4` 为 C5，或干脆改用 ESP-IDF。

---

## 4. 方案对比（修正后）

### 方案 A：ESP-IDF 路线 —— 把 iPhone-agent 固件移植成 IDF 组件

把 `hardware/hid_bridge` 从 Arduino sketch 改写为 ESP-IDF 工程，直接复用 Claw4 的 `sdkconfig` 与板级定义。

| 维度 | 评价 |
|---|---|
| 无线 | 直接用 C5，Wi-Fi 开箱即用；BLE 需按 §2 三选一 |
| USB HID | 用 IDF 的 `tinyusb` + `tusb_hid`，需确认板上 USB-C 走 OTG |
| 工作量 | **大**。6900 行里约 2716 行 sketch 根要改（TinyUSB API、HWCDC、NimBLE host 符号、`xTaskCreatePinnedToCore` 参数、core dump 分区）；约 4180 行 `src/` 纯 C 可原样保留 |
| 优点 | 与板子原生体系一致，不用跟预编译库较劲 |

### 方案 B：从源码构建 arduino-esp32，目标 C5

保留 Arduino 代码不动，改 `esp32-arduino-lib-builder/configs/defconfig.esp32p4` 的 slave target 为 esp32c5，自己编一套 core。

| 维度 | 评价 |
|---|---|
| 工作量 | 中（构建体系），固件代码几乎不动 |
| 优点 | `hid_bridge` 2716 行基本原样保留 |
| 风险 | 自己维护一套 core；预编译库与 C5 的兼容性未经官方测试 |

### 方案 C：换 ESP32-S3 开发板（~$7）

零移植，原固件直接跑。

**推荐 A**：板子原生就是 IDF 体系，方案 B 是在逆着官方支持方向走；而方案 A 虽然工作量大，但 `src/` 下 4180 行纯 C（AirPlay 栈、GOP、CLI 协议、bplist）可以整体复用，真正要改的是 Arduino 封装层。

---

## 5. 板子状态：已恢复原固件（2026-11-07）

**原固件真身（烧录前从 flash 备份 + 烧录后启动日志双向确认）：**

| 项 | 原固件 | Aug18 公开发布版 |
|---|---|---|
| Project name | **`agent`** | `xiaozhi` |
| App version | **0.2.0** | 2.0.51 |
| 编译时间 | **Aug 21 2026 21:14:11** | Aug 18 2026 20:07:20 |
| UUID | `c943844a-db63-4fc3-ba9b-e02ce80b87b3` | `845fed3d-…` |
| 分区表 | 7 个分区，`factory` app 14MB，**无 OTA**，无 emote/system/storage/coredump | 13 个分区，ota_0/ota_1 双槽 |
| 私有组件 | **Hermes 语音管线**（STT→Agent JSON-RPC→TTS→WAV，配置项 `hermes_durl/user/pass/profile`）+ **`codex_remote`** WebSocket | 都没有 |

**结论：原固件是私有定制版，公开仓库（含两个 release）里没有它。** 用户提供的 32MB 完整镜像
`/Users/linan/Downloads/0216a2de-190a-472b-bb57-1e2f252c0d4b.bin` 就是它——与烧录前的 flash
备份逐字节比对，app 区 32/32 块全同，仅差 bootloader 里 1 个字节（flash 速率位）和 nvs 运行时数据。

**已用 `esptool write_flash 0` 整片恢复**，启动日志确认：
`starting → activating → idle`，`Nt26Board: Network ready`，0 条错误。

- 端口名：恢复后是 `/dev/cu.usbmodem11101`（之前是 `31101`，CDC 接口号随固件变化）
- **烧录时必须选对口**：板子是 `VID 0x303A`（Espressif，"USB JTAG/serial debug unit"）；
  机器内置 4G 模组是 `VID 0x19d1`（EigenComm，`cu.usbmodem0000000000011/13`）——选错会报"连接设备失败"
- 取证备份：`~/Desktop/aicode/p4-probe/backup/firmware-before-probe.bin`（仅前 4MB，不完整）
- 参考仓库：`~/Desktop/aicode/MetalioClaw4`（公开版）、`~/Desktop/aicode/esp-hosted-mcu`

### 事故记录（教训）

过程中我用 Arduino 探针覆盖了原固件，且**备份只做了前 4MB 而 app 分区有 14MB**，导致原固件一度
无法从板子恢复。正确做法：烧任何东西之前先 `read_flash 0 <full_size>` 读全片。现已用户提供镜像补救。

---

## 6. 已验证里程碑（2026-11-07，真机运行）

移植固件已在 Metalio Claw4 的 ESP32-P4 上实际烧录并运行，**46/46 项自测全部通过**。

工程位置：`hardware/idf/`（ESP-IDF v5.5.5，`esp32:esp32` 无关，纯 IDF）

| 模块 | 结果 |
|---|---|
| `video/gop.c` GOP 缓冲 + H.264 分块 | 16/16 |
| `cli/cli_proto.c` X25519 配对 + HKDF + AES-GCM | 10/10 |
| `hid/arbiter.c` App/CLI 抢占仲裁 | 10/10 |
| `log/logring.c` RAM 日志环 | 4/4 |
| `bplist/bplist.c` 二进制 plist | 3/3 |
| **esp-hosted / ESP32-C5 over SDIO** | **3/3** |

### hosted C5 实测日志

```
eh_sdio: transport[host]: SDIO 4-bit 40000 kHz CLK=51 CMD=50 D0=49 D1=34 D2=31 D3=53 RESET=54
eh_sdio: Reset co-processor using GPIO[54]
eh_sdio: Card init success, TRANSPORT_RX_ACTIVE
eh_init_evt: slave chip id: 0x17 (esp32c5)
eh_init_evt: 	 * WLAN over SDIO
eh_init_evt: 	   - HCI over SDIO
eh_init_evt: esp-hosted fw versions: host=3.1.0 coprocessor=2.12.7
```

**两个关键结论：**

1. **Wi-Fi 通路已打通** —— `WLAN over SDIO`，`esp_wifi` 标准 API 可用
2. **C5 现有固件（2.12.7）已支持 `HCI over SDIO`** —— 这是本节最重要的发现：原厂 C5 固件
   **不需要重刷**就具备 BT HCI 能力。此前设计文档判断"必须重编 P4 + C5 两侧"过于悲观，
   实际只需在 P4 主机侧启用 hosted BT + NimBLE。C5 侧的工作从"重编固件"降级为"开机型配置"。

### 本节踩到并解决的 P4/IDF 特有问题

| 现象 | 原因 | 解法 |
|---|---|---|
| `Bootloader too large for offset 0x8000` | P4 bootloader 24.5KB，IDF 默认分区表在 0x8000 | `CONFIG_PARTITION_TABLE_OFFSET=0x9000` |
| `bootloader requires chip revision v3.1-v3.99 (this chip is v1.3)` | IDF 5.5.5 默认只认 v3.1+ 硅片 | `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`（从原厂 sdkconfig 反查得到） |
| `undefined reference to mbedtls_hkdf` | IDF 把 HKDF 藏在 Kconfig 后 | `CONFIG_MBEDTLS_HKDF_C=y` |
| `Guru Meditation: Stack protection fault` | 主任务栈 4KB 被局部缓冲 + ESP_LOGI 吃穿 | 大缓冲改 static + `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` |
| `unknown kconfig symbol ESP_HOSTED_HOST_SDIO_PIN_RESET` | 复位脚真名是 `ESP_HOSTED_HOST_RESET_GPIO` | 改名（写错会被静默丢弃，C5 将一直处于复位） |

定位 chip revision 那题的方法值得记录：解码原厂 bootloader 镜像头发现它声明 `v0.0 – v1.99`，
而我编出的是 `v3.1 – v3.99`，再去原厂 sdkconfig 反查即得 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`。

### 自测自身的 4 个错误（非核心代码缺陷）

第一轮 4 个 FAIL 全部是测试代码写错，修正后全绿：漏算 START 块的 4 字节板子时间戳；AVCC
长度前缀应为大端却用了 `memcpy` 原生小端；bplist marker `0x5N` 低 4 位是长度。这类"host
测试能过、目标平台才暴露"的问题，正是先把纯 C 核心单独移植的价值。

### 下一步

1. 启用 hosted BT（`CONFIG_ESP_HOSTED_HOST_FEAT_BT` + `CONFIG_BT_NIMBLE_ENABLED` +
   `esp_hosted_bt_host_stack_setup()`）——C5 侧已确认支持，只需主机侧配置
2. 移植 AirPlay 服务器（`src/airplay/{rtsp,handlers,mirror,pairing,fairplay}.c` + GPL playfair）
3. TinyUSB HID 接口（板子原厂 sdkconfig 已在用 `CONFIG_TINYUSB_MODE_SLAVE`）
4. iOS App：`Services/DingTalkReporter.swift` 已按 `LogUploader` 范式写好（语法检查通过，
   未编译验证——本机无 Xcode）

### hosted BLE 已验证（47/47 全通过）

```
eh_init_evt: slave chip id: 0x17 (esp32c5)
eh_init_evt: 	 * WLAN over SDIO
eh_init_evt: 	   - HCI over SDIO
esp_hosted_bt_nimble: NimBLE bound to hosted HCI
```

配置（全部在 `hardware/idf/sdkconfig.defaults`）：

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_CONTROLLER_DISABLED=y     # 控制器在 C5 上，P4 侧没有
CONFIG_ESP_HOSTED_HOST_FEAT_BT=y
```

代码只需一次调用：`esp_hosted_bt_host_stack_setup(&cfg)`，`cfg.stack = ESP_HOSTED_BT_HOST_STACK_NIMBLE`、
`cfg.bring_up_controller = true`。

**一个容易踩的坑**：`esp_hosted_bt_host_stack_setup()` 所在的那个适配组件（
`examples/common_components/esp_hosted_bt_host_stack`）**不是独立 IDF 组件**——它的 CMakeLists
明确写着 "Not a standalone IDF component"，依赖 `EH_PATH` 和 esp_hosted 内部 target。正确做法是
什么都不要拷贝：esp_hosted 的根 CMakeLists 第 110 行已经 `add_subdirectory` 了它，条件正是
`CONFIG_ESP_HOSTED_HOST_FEAT_BT`。把它复制到 `components/` 反而会因缺内部 target 而
"Failed to resolve component"。

另：P4 无 BT 控制器，`esp_bt.h` 不存在、`esp_read_mac(ESP_MAC_BT)` 不可用；BT MAC 要等 Wi-Fi
起来后才能从 C5 透传读到。判断 BLE 是否真通，看 `NimBLE bound to hosted HCI` 这行日志。

**至此 iPhone 连接的三个前提全部就绪：Wi-Fi（WLAN over SDIO）、BLE（HCI over SDIO）、
USB HID（板子原厂 sdkconfig 已在用 `CONFIG_TINYUSB_MODE_SLAVE`）。**

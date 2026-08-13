# Lanlink 项目交接文档(HANDOFF)

> 生成日期:2026-08-13 · 交接给下一个 agent 的完整上下文

> ## ✅ 核心问题已解决（2026-08-13 晚）
>
> 本文档「五、核心未解决问题」里描述的 **WiFi"链接效应"卡死/Cache error**，根因是 **`main` 任务栈溢出**（`I2sMic` 的 `m_raw[640]` 2560B + `pcm[640]` 1280B 超过默认 3584B 栈）。
>
> 已修复：大缓冲移出栈（`static`）+ `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`。
>
> 完整根因复盘见 **[STACK_OVERFLOW.md](STACK_OVERFLOW.md)**。下文其余内容保留作历史记录，其中"三大问题""链接效应"等结论均为栈溢出的假象。

## 一、项目概述

基于 **ESP-IDF v6.0.2** 的 ESP32-S3 语音转写(S2T)项目,名为 `Lanlink`。
核心链路:**INMP441 麦克风 → I2S 采集 → WiFi → WebSocket → 讯飞 RTASR 实时语音转写**。

## 二、环境与硬件

| 项 | 值 |
|---|---|
| SDK | ESP-IDF **v6.0.2**(`D:/esp/.espressif/v6.0.2/esp-idf`,当前最新发行版) |
| 目标芯片 | `esp32s3`(双核 Xtensa LX7) |
| 板子 | **n16r8**:16MB Flash + 8MB Octal PSRAM |
| 串口 | COM11,115200 |
| 编译器优化 | `-Os`(Size) |
| 构建 | VS Code ESP-IDF 扩展(`espIdfCommands`) |

**引脚:**
- INMP441:`BCLK=GPIO5`、`WS=GPIO4`、`DIN=GPIO8`(L/R 接 GND,左声道,24bit 左对齐)
- 按键:`GPIO12`(active_level=1)

## 三、代码结构

```
main/
├── main.cpp                    # app 入口,三个功能开关(RTASR/MIC/BUTTON)
├── CMakeLists.txt              # SRCS + REQUIRES
├── idf_component.yml           # 托管组件依赖:espressif/esp_websocket_client
├── apikey.h                    # 讯飞凭据(已 gitignore)
└── drivers/
    ├── wifi.cpp / wifi.hpp     # WiFi STA 驱动(官方 station 例程重构)
    ├── i2s_mic.cpp / i2s_mic.hpp  # INMP441 麦克风驱动
    ├── button.cpp / button.hpp # 多实例按键驱动
    └── rtasr.cpp / rtasr.hpp   # 讯飞 RTASR 客户端(官方 esp_websocket_client)
```

## 四、当前进度(已完成 ✅)

1. **WiFi 驱动** ✅ — 按官方 station 例程重构:事件组 + `xEventGroupWaitBits` 阻塞等待,断线自动重连(最多 10 次)。**纯 WiFi 最小版在 v6.0.2 下能稳定连接**。
2. **INMP441 驱动** ✅ — Philips 模式、32bit 槽位读 24bit、`>>16` 转 s16le,16kHz/16bit/单声道,每 40ms 1280 字节。`read_frame` 带 100ms 超时(不永久阻塞)。
3. **按键驱动** ✅ — 多实例、50ms 消抖。
4. **RTASR 客户端** ✅ — 用官方 `esp_websocket_client` 组件,ws://(不用 TLS),签名 `base64(HmacSHA1(MD5(appid+ts), api_key))` 用 PSA 计算,**已修复"两次 time(NULL)"的签名 bug(统一 ts)**。
5. **SNTP 时间同步** ✅ — `esp_netif_sntp` + pool.ntp.org。

## 五、核心未解决问题 ⚠️(最重要)

### WiFi "链接效应":链接进某些组件后,WiFi 固件静默卡死

**现象**:`esp_wifi_connect()` 之后**毫无反应**(日志停在 `wifi_init_sta finished, 等待连接 ...`,连 `new:<ch,0>` 都不打)。

**规律(多次实测)**:
| 固件组成 | WiFi 结果 |
|---|---|
| 纯 WiFi 最小版 | ✅ 能连 |
| + button | ✅ 能连 |
| + rtasr(官方 esp_websocket_client) | ✅ 能连(跑到鉴权) |
| + i2s(esp_driver_i2s + DMA) | ❌ 卡死 |
| 自研 esp_transport_ws 版(含 i2s,旧) | ✅ 曾连(IP 192.168.5.23) |

**已排除的方向(都有实测证据)**:
- ❌ IRAM/Flash 被组件占满(map 分析:ws/i2s 组件几乎不占 IRAM,Flash 也只 ~27KB)
- ❌ 全局构造器干扰(.init_array 里无 ws/i2s 组件)
- ❌ WiFi 附加功能配置(关掉 WPA3/SAE/GMAC/SoftAP/Enterprise 后仍卡)
- ❌ Size 面板 100% 满(那是旧固件缓存,实际没满)

**结论**:v6.0.2(最新发行版)的 WiFi 固件对"链接进的组件/固件大小"存在深层、疑似间歇性 bug。**没有更高版本可升**。

### 当前正在验证的方向
刚把 **指令缓存 32KB 撤销回 16KB**(`CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE=0x4000`),完整版(含 i2s)固件 0xbc070(770KB)。**待烧录测试 WiFi 能否连**。
- 若连上 → 指令缓存 32KB 是元凶
- 若还卡 → 继续减固件(降日志级别 `CONFIG_LOG_DEFAULT_LEVEL`、关断言、关 console 等)

## 六、关键决策与约定

1. **必须用官方 `esp_websocket_client` 组件**(用户明确要求,后续有大量 ws 需求),**不要回滚到自研 esp_transport_ws**。
2. **只用 ws://,不用 wss/TLS**(固件小,且 TLS 曾破坏 WiFi)。
3. **n16r8 配置**:16MB Flash + `partitions_singleapp_large.csv`(1500K app)+ 8MB Octal PSRAM(`CONFIG_SPIRAM=y, MODE_OCT, SPEED_80M`)。
4. **WiFi 任务固定核 1**(`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1=y`)。

## 七、sdkconfig 当前状态(相对默认的改动)

**保留的(n16r8)**:16MB flash、大分区、Octal PSRAM、WiFi 核1。

**已关闭(减固件)**:`ESP_WIFI_IRAM_OPT=n`、`RX_IRAM_OPT=n`、`WPA3_SAE=n`、`WPA3_OWE=n`、`GMAC=n`、`SOFTAP=n`、`ENTERPRISE=n`。

**已开启(优化)**:`COMPILER_OPTIMIZATION_SIZE=y`、`MBEDTLS_DYNAMIC_BUFFER=y`、`MBEDTLS_EXTERNAL_MEM_ALLOC=y`、`ESP_WIFI_DYNAMIC_RX_MGMT_BUFFER=y`、`SPIRAM_MALLOC_ALWAYSINTERNAL=16384`。

**刚撤销**:指令缓存 32KB → **16KB**。

## 八、待办清单

1. [ ] 烧录测试:指令缓存 16KB 下,完整版(含 i2s)WiFi 能否连上
2. [ ] 若还卡 → 继续减固件(降日志级别/关断言/关 console)或深入查根因
3. [ ] 签名修好后,验证 RTASR 能拿到 `[中间]/[最终]` 转写结果(之前卡在 10110 illegal signa,已修)
4. [ ] 解决串口乱码(`Failed to decode` — 疑似 XTAL/波特率配置,和 i2s 无关)
5. [ ] 完整 S2T 流程联调:说话 → 实时出字

## 九、常见坑(踩过的)

- **IDF 版本切换后必须 `fullClean`**,否则 bootloader 缓存路径不匹配报错。
- **mbedtls v4(PSA API)**:MD5 用 `psa_hash_compute`,HMAC-SHA1 用 `psa_mac_compute`(先 `psa_import_key`),SHA1 宏是 `PSA_ALG_SHA_1`。
- **v6.0.2 无内置 `esp_websocket_client`/`cJSON`**:用 `idf_component.yml` 拉取托管组件。
- **CMake REQUIRES**:`main` 组件不写 REQUIRES 默认依赖所有;写了就只依赖列出的 + common。
- **Werror**:结构体设计化初始化要 `= {}` 或补齐字段(如 `i2s_std_gpio_config_t` 的 `invert_flags`)。
- **`CONFIG_FREERTOS_HZ=100`**:1 tick=10ms,`vTaskDelay` 必须 ≥10ms。
- **`ESP_RETURN_ON_ERROR`** 需 `#include "esp_check.h"`。
- **修改 sdkconfig 文本后**,Kconfig 重配会用 sdkconfig 值(有 default mismatch 提示可忽略);但 **`setTarget` 会重置 sdkconfig**,改配置用 `sdkconfig.defaults` 固化更稳。
- **串口被多个终端占用**会报 `PermissionError` 并污染 monitor 输出。

## 十、移交提示词(可直接发给新 agent)

见下方回复中的"移交提示词"段落。

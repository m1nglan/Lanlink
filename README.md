# Lanlink

基于 **ESP32-S3** 的实时语音转写（S2T）设备：**INMP441 麦克风 → I2S 采集 → WiFi → WebSocket → 讯飞 RTASR 实时语音转文字**。

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.0.2-blue)](https://docs.espressif.com/projects/esp-idf/)
[![Target](https://img.shields.io/badge/Target-ESP32--S3-green)](#硬件)

## 功能

- **音频采集**：INMP441 数字麦克风，I2S 标准模式，16kHz / 16bit / 单声道
- **WiFi 连接**：STA 模式，自动重连
- **语音转写**：通过 `ws://`（不加密）流式上传 PCM 到讯飞实时语音转写（RTASR），实时返回识别文本

数据链路：

```
INMP441 ──I2S──▶ 16k/16bit/mono PCM ──每40ms/1280字节──▶ WebSocket ──▶ 讯飞 RTASR ──▶ 识别文本
```

## 硬件

| 项 | 值 |
|---|---|
| 主控 | ESP32-S3（n16r8：16MB Flash + 8MB Octal PSRAM） |
| 麦克风 | INMP441（I2S 数字麦克风） |
| 按键 | 1 个（GPIO12，按下为高） |

**INMP441 接线：**

| INMP441 | ESP32-S3 GPIO |
|---|---|
| SCK（BCLK） | GPIO5 |
| WS（LRCK） | GPIO4 |
| SD（DIN） | GPIO8 |
| L/R | 接 GND（左声道） |

> 引脚定义在 [main/drivers/i2s_mic.hpp](main/drivers/i2s_mic.hpp)，按键在 [main/drivers/button.hpp](main/drivers/button.hpp)。

## 软件环境

- **ESP-IDF v6.0.2**（`D:/esp/.espressif/v6.0.2/esp-idf`）
- 目标芯片 `esp32s3`
- 托管组件：`espressif/esp_websocket_client`（见 [main/idf_component.yml](main/idf_component.yml)）

## 快速开始

### 1. 配置凭据

**WiFi**：修改 [main/drivers/wifi.hpp](main/drivers/wifi.hpp) 里的 `WIFI_SSID` / `WIFI_PASSWORD`。

**讯飞**：在 [main/apikey.h](main/apikey.h) 填入 `XFYUN_APPID` / `XFYUN_API_KEY`（该文件含密钥，**不要提交到 git**）。

### 2. 构建 & 烧录

```bash
idf.py set-target esp32s3   # 首次配置
idf.py build                # 构建
idf.py -p COM11 flash       # 烧录
idf.py -p COM11 monitor     # 串口监控（115200）
```

> 也可以直接在 VS Code 用 ESP-IDF 扩展的 build / flash / monitor 命令。

### 3. 运行

上电后自动：连 WiFi → 初始化麦克风 → 连接讯飞 RTASR → 说话即可看到实时转写文本（`[中间]` / `[最终]`）。

## 项目结构

```
main/
├── main.cpp                  # 入口，含 RTASR/MIC/BUTTON 三个功能开关
├── CMakeLists.txt            # 组件注册
├── idf_component.yml         # esp_websocket_client 依赖
├── apikey.h                  # 讯飞凭据（gitignore，勿提交）
└── drivers/
    ├── wifi.{hpp,cpp}        # WiFi STA 驱动（事件组阻塞等待连接）
    ├── i2s_mic.{hpp,cpp}     # INMP441 I2S 麦克风驱动
    ├── button.{hpp,cpp}      # 按键驱动（软件消抖）
    └── rtasr.{hpp,cpp}       # 讯飞 RTASR 客户端（官方 esp_websocket_client）
```

## 关键配置（sdkconfig.defaults）

| 配置 | 说明 |
|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | n16r8 板 16MB Flash |
| `CONFIG_SPIRAM` + Octal @80MHz | n16r8 板 8MB PSRAM |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | ⚠️ **必须**，见下方注意事项 |

## ⚠️ 重要注意事项

**`main` 任务栈必须保持 ≥ 8KB**。历史上曾因 `I2sMic` 的大缓冲（`m_raw[640]` 2560B + `pcm[640]` 1280B）压在默认 3584B 栈上导致**栈溢出**，引发一系列"诡异"假象（WiFi 卡死、Cache error、扫描阻塞）。

**教训**：大缓冲（>1KB）一律用 `static` 或 `malloc`，不要放栈上。

完整根因复盘见 **[STACK_OVERFLOW.md](STACK_OVERFLOW.md)**。

## 相关文档

- [STACK_OVERFLOW.md](STACK_OVERFLOW.md) — 栈溢出根因复盘（必读）
- [problem.md](problem.md) — 历史问题排查记录
- [HANDOFF.md](HANDOFF.md) — 项目交接文档
- [AGENTS.md](AGENTS.md) — 项目开发指南

## License

（待补充）

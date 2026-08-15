# Lanlink

基于 **ESP32-S3** 的实时语音转写（S2T）设备：**INMP441 麦克风 → I2S 采集 → WiFi → WebSocket → 自建语音网关 → 讯飞实时语音转文字**。

支持**按键长按录音、实时流式出字、多轮会话**。

## 功能

- **实时语音转写**：说话时逐字实时输出（`识别中:`），松开后输出完整结果（`识别结果:`）
- **按键控制**：长按 0.5s 开始录音，松开结束（防误触，短按忽略）
- **长连接保活**：应用层 ping 保活 + 死连接自动重连
- **多轮会话**：一轮识别完成后可立即进行下一轮
- **可扩展 LLM**：内置 LLM/OpenClaw 对话驱动（待接入 main）

数据链路：

```
INMP441 ──I2S──▶ PCM(16k/16bit/mono) ──StreamBuffer──▶ WebSocket ──▶ 自建网关 ──▶ 讯飞
                                                                       │
按键长按0.5s ──▶ start ──▶ 实时发音频 ──▶ 松开end ──▶ final ◀── 识别文本回传
```

## 硬件

| 项 | 值 |
|---|---|
| 主控 | ESP32-S3（n16r8：16MB Flash + 8MB Octal PSRAM） |
| 麦克风 | INMP441（I2S 数字麦克风） |
| 按键 | GPIO10（按下为高，外部下拉） |

**INMP441 接线：**

| INMP441 | ESP32-S3 GPIO |
|---|---|
| SCK（BCLK） | GPIO11 |
| WS（LRCK） | GPIO12 |
| SD（DIN） | GPIO13 |
| L/R | 接 GND（左声道） |

> 引脚定义在 [main/drivers/i2s_mic.hpp](main/drivers/i2s_mic.hpp)、[main/main.cpp](main/main.cpp)。

## 软件环境

- **ESP-IDF v6.0.2**（`D:/esp/.espressif/v6.0.2/esp-idf`）
- 目标芯片 `esp32s3`
- 托管组件：`espressif/esp_websocket_client`、`espressif/cjson`（见 [main/idf_component.yml](main/idf_component.yml)）

## 语音网关

本项目使用**自建转发服务**（`ws://39.104.84.177:18888`）转发板子音频到讯飞，板子不直接调讯飞 API（无需在板子上做鉴权签名）。

- 协议：WebSocket，一条长连接，应用层切换服务（text / llm / openclaw）
- 鉴权：URL 参数 `token`（见 [main/apikey.h](main/apikey.h) 的 `SERVER_TOKEN`，该文件已 gitignore）
- 协议文档：见 [APIserver.md](APIserver.md)

## 快速开始

### 1. 配置凭据

在 [main/apikey.h](main/apikey.h) 填入网关 token（`SERVER_TOKEN`）。该文件含敏感信息，**不要提交到 git**（已 `.gitignore`）。

### 2. 构建 & 烧录

```bash
idf.py set-target esp32s3   # 首次配置
idf.py build                # 构建
idf.py -p COM11 flash       # 烧录
idf.py -p COM11 monitor     # 串口监控（115200）
```

> 也可以在 VS Code 用 ESP-IDF 扩展。

### 3. 使用

1. 上电 → 自动连 WiFi → 连网关（长连接保持）
2. **长按按键 0.5s** → 开始录音，说话 → **实时看到识别字**（`识别中: ...`）
3. **松开** → 输出完整结果（`识别结果: ...`）→ 可立即进行下一轮

## 项目结构

```
main/
├── main.cpp                  # 入口：三任务架构 + 录音状态机 + 长按检测
├── CMakeLists.txt            # 组件注册
├── idf_component.yml         # esp_websocket_client / cjson 依赖
├── apikey.h                  # 网关 token（gitignore，勿提交）
└── drivers/
    ├── wifi.{hpp,cpp}        # WiFi STA 驱动（断线自动重连）
    ├── i2s_mic.{hpp,cpp}     # INMP441 I2S 麦克风驱动
    ├── button.{hpp,cpp}      # 按键驱动（软件消抖）
    ├── ws.{hpp,cpp}          # 通用 WebSocket 长连接驱动（单例，粘包处理 + 按 type 分发）
    ├── rtasr.{hpp,cpp}       # 语音听写业务驱动（start/audio/end + 结果累积）
    └── llm.{hpp,cpp}         # LLM/OpenClaw 对话驱动（待接入 main）
```

### 双核任务架构

| 核 | 任务 | 职责 |
|---|---|---|
| CPU0 | `ws_task` | WiFi 连接 + WebSocket 长连接 + 发 start/音频/end + 定时 ping 保活 |
| CPU1 | `button_task` | 按键检测（长按 0.5s 确认）+ 录音状态机 |
| CPU1 | `i2s_task` | I2S 采集 PCM → 写 Stream Buffer |

**跨任务通信**：
- 音频：i2s_task → `StreamBuffer` → ws_task
- 状态：`volatile` 状态机（IDLE / RECORDING / WAITING），ws_task 读状态决定发什么

### 录音状态机

```
IDLE ──长按0.5s──▶ RECORDING ──松开──▶ WAITING ──收到final──▶ IDLE
                                                              ▲
                                             10s无final超时兜底 ─┘
```

- **IDLE**：空闲，长按开始录音
- **RECORDING**：录音中，实时发音频
- **WAITING**：已发 end，等 final（此期间不可再录音）
- final 回 IDLE；若 10s 没收到 final（服务器异常），超时兜底强制回 IDLE

## 关键配置（sdkconfig.defaults）

| 配置 | 说明 |
|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | n16r8 板 16MB Flash |
| `CONFIG_SPIRAM` + Octal @80MHz | n16r8 板 8MB PSRAM |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | ⚠️ **必须**，见下方注意事项 |

## ⚠️ 重要注意事项

**`main` 任务栈必须保持 ≥ 8KB**。历史上曾因 `I2sMic` 的大缓冲（2560B + 1280B）压在默认 3584B 栈上导致**栈溢出**，引发 WiFi 卡死、Cache error、扫描阻塞等一系列"诡异"假象。

**教训**：大缓冲（>1KB）一律用 `static` 或 `malloc`，不要放栈上。

完整根因复盘见 **[STACK_OVERFLOW.md](STACK_OVERFLOW.md)**。

## 相关文档

- [APIserver.md](APIserver.md) — 语音网关接口协议（V1.1 长连接）
- [STACK_OVERFLOW.md](STACK_OVERFLOW.md) — 栈溢出根因复盘（必读）
- [problem.md](problem.md) — 历史问题排查记录
- [HANDOFF.md](HANDOFF.md) — 项目交接文档
- [AGENTS.md](AGENTS.md) — 项目开发指南

## License

（待补充）

# Lanlink

基于 **ESP32-S3** 的实时语音转写（S2T）设备：**INMP441 麦克风 → I2S 采集 → WiFi → WebSocket → 自建语音网关 → 讯飞实时语音转文字**。

支持**按键长按录音、实时流式出字、多轮会话**。

## 功能

- **实时语音转写**：说话时流式逐字输出，松开后输出完整结果
- **语音修正**：讯飞修正时 `[修正]` 换行重打整条，保持中间显示准确
- **按键控制**：IO10 长按 0.5s 开始录音，松开结束（防误触）；IO8 按下切换 LLM/OpenClaw 服务
- **自动转发 LLM**：识别完成后自动把文本发给当前选中的服务（OpenClaw 明岚 / LLM DeepSeek）
- **流式回复**：LLM 回复流式逐字显示，完整 reply 换行定格
- **长连接保活**：应用层 ping 保活 + 死连接自动重连（阈值 130s，兼容 LLM 长空窗）
- **多轮会话**：一轮完成后自动切回 text，可立即进行下一轮

数据链路：

```
INMP441 ──I2S──▶ PCM(16k/16bit/mono) ──StreamBuffer──▶ WebSocket ──▶ 自建网关 ──▶ 讯飞
                                                                       │
IO10长按0.5s ──▶ start ──▶ 实时发音频 ──▶ 松开end ──▶ final ──▶ 自动转发到 LLM/OpenClaw
```

## 硬件

| 项 | 值 |
|---|---|
| 主控 | ESP32-S3（n16r8：16MB Flash + 8MB Octal PSRAM） |
| 麦克风 | INMP441（I2S 数字麦克风） |
| 录音键 | GPIO10（按下为低，内部上拉） |
| 切换键 | GPIO8（按下为低，内部上拉，切 LLM/OpenClaw） |

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
2. **长按 IO10 0.5s** → 开始录音，说话 → 流式看到识别字
3. **松开** → 输出完整结果（`[识别] ...`）→ **自动转发给当前服务**（默认 OpenClaw）
4. 看到 `[OpenClaw] ...` 流式回复 → 完整 reply 换行定格 → 切回 text
5. **按 IO8** → 切换服务（`切换到 llm` / `切换到 openclaw`），之后识别转发的目标随之改变

## 项目结构

```
main/
├── main.cpp                  # 入口：三任务创建 + 回调接线（精简，业务在 app_fsm）
├── CMakeLists.txt            # 组件注册
├── idf_component.yml         # esp_websocket_client / cjson 依赖
├── apikey.h                  # 网关 token（gitignore，勿提交）
└── drivers/
    ├── app_fsm.{hpp,cpp}     # 应用状态机模块（录音 + LLM 阶段机 + 服务选择）
    ├── wifi.{hpp,cpp}        # WiFi STA 驱动（断线自动重连）
    ├── i2s_mic.{hpp,cpp}     # INMP441 I2S 麦克风驱动
    ├── button.{hpp,cpp}      # 按键驱动（消抖 + 按下沿检测 is_pressed_edge）
    ├── ws.{hpp,cpp}          # 通用 WebSocket 长连接驱动（单例，粘包处理 + 按 type/服务分发）
    ├── rtasr.{hpp,cpp}       # 语音听写业务驱动（partial/revise/final + 结果累积）
    └── llm.{hpp,cpp}         # LLM/OpenClaw 对话驱动（流式 partial + reply）
```

### 双核任务架构

| 核 | 任务 | 职责 |
|---|---|---|
| CPU0 | `ws_task` | WiFi + 长连接 + 语音收发 + LLM 转发（循环调 `AppFsm::tick`） |
| CPU1 | `button_task` | IO10 长按录音 / IO8 切换服务 |
| CPU1 | `i2s_task` | I2S 采集 PCM → 写 Stream Buffer |

**业务状态机集中在 `app_fsm`**（main.cpp 只做任务创建 + 回调接线）。

### 录音状态机

```
IDLE ──IO10长按0.5s──▶ RECORDING ──松开──▶ WAITING ──收到final──▶ IDLE
                                                              ▲
                                             10s无final超时兜底 ─┘
```

- **IDLE**：空闲，长按开始录音
- **RECORDING**：录音中，实时发音频
- **WAITING**：已发 end，等 final（此期间不可再录音）
- final 回 IDLE；若 10s 没收到 final，超时兜底强制回 IDLE

### LLM 转发阶段机（AppFsm 内部）

```
IDLE ──收到final──▶ SWITCHING ──切到选中服务──▶ CHATTING ──发chat等reply──▶ BACK ──切回text──▶ IDLE
```

- 识别 final 后自动发到当前选中服务（OpenClaw 默认 / LLM 可选）
- OpenClaw 回复最长等 **120s**（明岚工具调用空窗长）
- 收到 reply 或超时 → 切回 text，允许下一轮录音

### 服务切换（IO8）

```
IO8 按下 → openclaw(明岚) ↔ llm(DeepSeek) 切换
默认 OpenClaw；LLM 会话进行中不可切换
```

## 关键配置（sdkconfig.defaults）

| 配置 | 说明 |
|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | n16r8 板 16MB Flash |
| `CONFIG_SPIRAM` + Octal @80MHz | n16r8 板 8MB PSRAM |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | ⚠️ **必须**，见下方注意事项 |

## ⚠️ 重要注意事项

1. **`main` 任务栈必须保持 ≥ 8KB**。历史上曾因 `I2sMic` 的大缓冲（2560B + 1280B）压在默认 3584B 栈上导致**栈溢出**，引发 WiFi 卡死、Cache error、扫描阻塞等一系列"诡异"假象。
   - **教训**：大缓冲（>1KB）一律用 `static` 或 `malloc`，不要放栈上。

2. **录音必须在 text 服务下进行**。识别前会强制 `switch_service("text")`，确保服务器处理 start/音频。

3. **LLM 空窗长**（OpenClaw 工具调用可能几十秒），`WS_STALE_TIMEOUT_MS=130s` 已调大避免误判断连。

完整根因复盘见 **[STACK_OVERFLOW.md](STACK_OVERFLOW.md)**。

## 相关文档

- [APIserver.md](APIserver.md) — 语音网关接口协议（V1.1 长连接）
- [STACK_OVERFLOW.md](STACK_OVERFLOW.md) — 栈溢出根因复盘（必读）
- [problem.md](problem.md) — 历史问题排查记录
- [HANDOFF.md](HANDOFF.md) — 项目交接文档
- [AGENTS.md](AGENTS.md) — 项目开发指南

## License

（待补充）

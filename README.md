# Lanlink
基于 **ESP32-S3** 的实时语音助手设备：**INMP441 麦克风 → I2S 采集 → WiFi → WebSocket → 自建语音网关 → 讯飞实时语音转写 → 转发大模型 → 条屏显示**。

**按住说话 → 聊天屏逐字出字 → 松开定格 → 自动转发给明岚(OpenClaw) → 回复显示在对方气泡里。**

> 📌 **架构说明看 [`MD/CHAIN.md`](MD/CHAIN.md)** —— 分层、职责边界、五条链、队列、全局约束。
> 协议细节看 [`MD/PROTOCOL.md`](MD/PROTOCOL.md)。

---

## 功能

- **实时语音转写**：说话时流式逐字上屏；松开后由服务器的 `final` 给出最终修正版
- **双向气泡**：我说的（绿底/靠右）+ 对方的回复（灰底/靠左）
- **按下即出气泡**：不等服务器回字，按下键就冒一个空气泡（否则前一两秒屏幕毫无反应）
- **自动转发大模型**：识别完成后自动把文本发到 OpenClaw（明岚），回复流式写进灰泡
- **`(思考中)` 占位**：明岚调工具时 3~25 秒才吐字，先放个占位灰泡，真回复一到自动覆盖
- **聊天历史**：换屏销毁后重进聊天屏，说过的话会重放出来（PSRAM 环形缓冲）
- **长连接保活**：应用层 ping + 死连接自动重连（阈值 130s，兼容大模型长空窗）
- **录音期间禁止转屏**：避免误触编码器打断会话

---

## 硬件

| 项 | 值 |
|---|---|
| 主控 | ESP32-S3（**n16r8**：16MB Flash + 8MB Octal PSRAM） |
| 麦克风 | INMP441（I2S 数字麦克风） |
| 屏幕 | ST7789 320×170 1.9" 条屏（SPI2） |
| 输入 | 旋转编码器 + 两个物理键 |

### 引脚（★★ 以代码为准）

| 外设 | 引脚 | 定义位置 |
|---|---|---|
| **ST7789 屏**（SPI2）| SCLK **9** ｜ MOSI **46** ｜ RST **3** ｜ DC **8** ｜ CS **18** ｜ BL **17** | [main/display/lcd_display.hpp](main/display/lcd_display.hpp) |
| **旋转编码器** | A **45** ｜ B **41** | [main/drivers/encoder.hpp](main/drivers/encoder.hpp) |
| **物理按键** | 录音 **2** ｜ 服务 **42**（均按下为低、内部上拉）| [main/main.cpp](main/main.cpp) |
| **INMP441**（I2S）| BCLK **21** ｜ WS **47** ｜ DIN **1** | [main/drivers/i2s_mic.hpp](main/drivers/i2s_mic.hpp) |

> ⚠️ **引脚改过好几轮，旧文档里的 `IO10`/`IO8`/`SCLK=21`/`BCLK=11` 全部作废。**
> 排查硬件前**先打开上面这些头文件对一遍实际连线**，别信记忆。一键列全部引脚：在 `main/` 里搜 `GPIO_NUM_`。

**INMP441 接线：**

| INMP441 | ESP32-S3 |
|---|---|
| SCK（BCLK） | GPIO21 |
| WS（LRCK） | GPIO47 |
| SD（DIN） | GPIO1 |
| L/R | 接 GND（左声道） |

### ⚠️ 用到的"特殊脚"（合法但要知道）

| 脚 | 特殊之处 | 用在哪 |
|---|---|---|
| **GPIO45 / GPIO46** | **strapping**（VDD_SPI 电压 / ROM 打印）| 编码器 A、屏 MOSI |
| **GPIO3** | **strapping**（JTAG 源选择）| 屏 RST |
| **GPIO39–42** | **JTAG 默认功能** | 编码器 B=41、服务键=42 |

- 当普通 GPIO 用**没问题**（前提是不接 JTAG 调试），**但上电瞬间有默认电平**
- **n16r8 的 Octal PSRAM 占 GPIO33–37，外设绝不能用**（上表没用到 ✓）

---

## 软件环境

- **ESP-IDF v6.0.2**（`D:/esp/.espressif/v6.0.2/esp-idf`）
- 目标芯片 `esp32s3`，C++（gnu++26）
- **LVGL 9.5.0**（registry 组件，**锁定**，SquareLine 1.6.2 导出目标，勿升级）
- 托管组件：`espressif/esp_websocket_client`、`espressif/cjson`（见 [main/idf_component.yml](main/idf_component.yml)）

---

## 语音网关

本项目使用**自建转发服务**（`ws://39.104.84.177:18888`）把板子音频转发到讯飞；板子**不直接调讯飞 API**（无需在板子上做鉴权签名）。

- 协议：WebSocket 单条长连接，应用层用 `{"type":"svc","service":...}` 切换服务（`text` / `llm` / `openclaw`）
- 鉴权：URL 参数 `token`（见 [main/apikey.h](main/apikey.h) 的 `SERVER_TOKEN`，**该文件已 gitignore**）
- **完整协议见 [`MD/PROTOCOL.md`](MD/PROTOCOL.md)**（V2）

### 协议要点（V2）

| 方向 | 报文 |
|---|---|
| 板子 → 网关 | `{"type":"start"}` / `{"type":"end"}` / `{"type":"svc","service":"…"}` / `{"type":"chat","content":"…"}` |
| 网关 → 板子 | `{"type":"asr","text":"<完整当前句>"}`（**全量替换**）/ `{"type":"final","text":"<完整>"}` |
| | `{"type":"partial","text":"<增量>"}` / `{"type":"reply","content":"<完整>"}`（大模型）|
| | `{"type":"abroad","text":"…"或 null}`（**讯飞那边先关了会话**）|
| | `{"type":"error","code":N}` |

**⚠️ `stream` 通道上的 `text` 永远是【完整文本】** —— 板子消费者**零累积状态**。
只有大模型的 `partial` 是增量，**累积在驱动侧（`Llm`）做完**才发布。

---

## 快速开始

### 1. 配置凭据

在 [main/apikey.h](main/apikey.h) 填入网关 token（`SERVER_TOKEN`）。含敏感信息，**不要提交**（已 `.gitignore`）。

### 2. 构建 & 烧录

```bash
idf.py set-target esp32s3   # 首次配置
idf.py build
idf.py -p COM11 flash
idf.py -p COM11 monitor     # 115200
```

> 也可以在 VS Code 用 ESP-IDF 扩展。

⚠️ **新增源文件后必须刷新 CMakeLists 的时间戳**（`SRC_DIRS` 是 `file(GLOB)`，只在 CMake 配置阶段扫描）：
```powershell
(Get-Item main\CMakeLists.txt).LastWriteTime = Get-Date   # Windows
# Linux/macOS: touch main/CMakeLists.txt
```
不刷新则新文件不参与编译，报链接期 `undefined reference`。

### 3. 使用

1. 上电 → 自动连 WiFi → 连网关（长连接保持）；屏幕先播启动动画（`ui_main`），约 7 秒淡入主界面
2. 转到聊天屏 → **按住录音键（GPIO2）** → 屏幕**立刻冒一个小绿泡**，说话时逐字增长
3. **松开** → 绿泡定格在最终识别结果；同时自动转发给 OpenClaw
4. 约 1 秒后灰泡出现 **`(思考中)`** → 约 20 秒后被明岚的回复覆盖
5. **录音期间转编码器无效**（有意为之，避免误触打断会话）

---

## 架构

### 模块结构

```
main/
├── main.cpp                    # 入口：初始化外设 + 任务创建 + 按键回调接线
├── CMakeLists.txt / idf_component.yml
├── apikey.h                    # 网关 token（gitignore）
│
├── business/                   # 业务层（不认识 LVGL）
│   ├── bus_msg.hpp             # ★ 纯类型：三条队列的消息结构 (voice/resp/stream)
│   ├── ui_bridge.{hpp,cpp}     # ★ 队列 + 回调接线 + 会话控制 + 消费分发
│   ├── ask_router.{hpp,cpp}    # ★ "何时把哪段文本发给谁"这条策略（靠注入回调，不认识队列/LVGL）
│   ├── voice.{hpp,cpp}         # voice_task：切服务→start→采音→end
│   └── ws_keeper.{hpp,cpp}     # 连接保活与重连（独占 init/deinit/重连）
│
├── display/                    # 显示层
│   ├── chat_view.{hpp,cpp}     # ★ 气泡创建/更新/销毁 + 生命周期状态机 + 历史环形缓冲
│   ├── lvgl_port.{hpp,cpp}     # LVGL 移植：任务/tick/锁/绘图缓冲/编码器 indev
│   └── lcd_display.{hpp,cpp}   # ST7789 面板初始化
│
├── drivers/                    # 协议/外设驱动（不认业务）
│   ├── ws.{hpp,cpp}            # 通用 WebSocket + 按 type 查表分发
│   ├── rtasr.{hpp,cpp}         # text 服务协议（asr / final）
│   ├── llm.{hpp,cpp}           # 对话服务协议（partial / reply）+ 增量累积
│   ├── i2s_mic.{hpp,cpp}       # INMP441 采集
│   ├── button_edge.{hpp,cpp}   # 按键：GPIO 中断 + esp_timer 消抖
│   ├── encoder.{hpp,cpp}       # 编码器：ISR + 查表解码
│   ├── wifi.{hpp,cpp}          # WiFi STA
│   └── gpio_isr_once.hpp       # GPIO ISR 服务只装一次的 helper
│
└── lvgl/                       # SquareLine 导出（UI 画面 + 字体 + 图片，勿手改）
```

**三个分层原则（详见 [`MD/CHAIN.md`](MD/CHAIN.md) §4）：**

| 模块 | **不该做什么** |
|---|---|
| `ui_bridge` | 不做显示、不做转发决策 |
| `ask_router` | **不认识 LVGL、不认识队列**（三个动作靠 `init()` 注入的回调）|
| `chat_view` | **不认识业务**（不知道 ASR / LLM / openclaw 的存在）|

### 三条队列

| 队列 | 深度 | 语义 | 走什么 | 消费者 |
|---|---|---|---|---|
| **`voice_q`** | 4 | 命令，不能丢 | `voice_cmd_t` | **只有 `voice_task`** |
| **`resp_q`** | 16 | 可靠，逐条处理 | 状态 + **短**文本（64B）| `drain_queues` |
| **`stream_q`** | **1** | **覆盖式**，只留最新 | 长文本（2048B）| `drain_queues` |

**总计 3304 字节内部 SRAM。**

**三条铁律：**
1. **WS 回调里只能非阻塞投递** —— 那时正持 `client->lock`，阻塞会拖死整个收发
2. **值拷贝原则** —— 队列靠 `memcpy` 搬字节，**载荷里绝不能有指针**
33. **`stream_q` 深度必须是 1** —— 长文本只占一份，`BUS_TEXT_LEN` 才敢开到 2048
### 任务布局

| 核 | 任务 | 优先级 | 栈 | 职责 |
|---|---|---|---|---|
| **CPU1** | `lvgl` | 2 | 6K | `lv_timer_handler` + 消费队列（`drain_queues`）|
| **CPU0** | `voice` | 6 | 6K | 语音会话：切服务→start→采音→end |
| **CPU0** | `ws_keeper` | 5 | 8K | 长连接保活与重连 |
| **CPU0** | `websocket_task` | 5 | 4K | 组件自带，收发 + 回调（**持 `client->lock`**）|

**按键和编码器没有任务** —— 按键是 GPIO 中断 + `esp_timer` 消抖；编码器是 GPIO 中断 + LVGL indev 回调。

### 一次完整问答的时序

```
 t=0ms    按下录音键 → [中断] 消抖 → on_button_edge → voice_start → voice_q
 t=10ms   voice_task: 等连接 → 切 text → 等 svc_ok（实测 50ms）
 t=60ms   asr.start + mic.start
 t=60ms   ★ 绿泡出现（空气泡）        ← STARTED 走 resp_q，立刻
 t=60ms~  asr 逐字增长 → 绿泡逐字刷新
 t=3.9s   松开 → mic.stop → asr.end → ENDED
 t=5.4s   final 到达（end + 1470ms）→ 绿泡显示最终版（带标点）
          同时 ask_router 立刻转发 → svc openclaw(31ms) → chat "[Lanlink] …"
 t=5.4s   ★ 灰泡 "(思考中)" 出现        ← 因为明岚要 20 秒才回
 t=5.4s   ★ 2.5s 宽限期到 → 绿泡定格
 t=26s    明岚 reply 到 → 灰泡文字被覆盖（占位自动消失）
```

---

## 关键配置（sdkconfig.defaults）

| 配置 | 说明 |
|---|---|
| `CONFIG_ESPTOOLPY_FLASHSIZE_16MB` | n16r8 板 16MB Flash |
| `CONFIG_SPIRAM` + `CONFIG_SPIRAM_MODE_OCT` + 80MHz | n16r8 板 8MB Octal PSRAM |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192` | ⚠️ **必须**，见下方注意事项 |
| `CONFIG_LV_USE_FONT_COMPRESSED=y` | ⚠️ **必须**，见下方注意事项 |
| `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` | <16KB 的分配优先落内部 SRAM |

---

## ⚠️ 重要注意事项

### 1. `main` 任务栈必须保持 ≥ 8KB

历史上曾因 `I2sMic` 的大缓冲（2560B + 1280B）压在默认 3584B 栈上导致**栈溢出**，引发 WiFi 卡死、Cache error、扫描阻塞等一系列"诡异"假象。

**教训：大缓冲（>1KB）一律用 `static` 或 `malloc`，不要放栈上。**

完整根因复盘见 [`MD/STACK_OVERFLOW.md`](MD/STACK_OVERFLOW.md)。

### 2. `CONFIG_LV_USE_FONT_COMPRESSED` 必须开

SquareLine 导出的中文字体**可能是压缩的** —— 看 `main/lvgl/fonts/ui_font_*.c` 里的 `.bitmap_format`：`1` = 压缩。

**⚠️ 症状极坑**：字体压缩但没开解压器时，LVGL 的**度量**照常读、**像素**直接 `return NULL`
→ **框的长度对、字一个都不画**。

⚠️ 文件头 `Opts:` 里的 `--no-compress` **是假的**，**以 `.bitmap_format` 为准**。

### 3. 录音必须在 `text` 服务下进行

识别前会强制 `switch_service("text")`，确保服务器正确处理 `start`/音频。

### 4. 大模型空窗长

明岚调工具时可能 **3~25 秒**不吐字，`WS_STALE_TIMEOUT_MS=130s` 已调大避免误判断连。

### 5. `pdMS_TO_TICKS(1..9) == 0`

`CONFIG_FREERTOS_HZ=100` → 1 tick = 10ms。**需要短超时要夹到 ≥1 tick**（否则 0 表示"不等待"）。

### 6. LVGL 非线程安全

非 LVGL 任务访问 UI 必须 `lvgl_port_lock()`；
**但 LVGL 上下文内（回调 / `lv_timer`）绝不能再加锁**（锁是非递归的，会自死锁）。

---

## 相关文档

| 文档 | 内容 |
|---|---|
| **[`MD/CHAIN.md`](MD/CHAIN.md)** | ★ **架构说明**：分层 / 职责边界 / 五条链 / 队列 / 全局约束 |
| **[`MD/PROTOCOL.md`](MD/PROTOCOL.md)** | ★ **协议 V2 完整规范**（改协议前必读）|
| [`AGENTS.md`](AGENTS.md) | 开发指南：环境、引脚、常见坑（**给 AI 助手看的**）|
| [`MD/STACK_OVERFLOW.md`](MD/STACK_OVERFLOW.md) | 栈溢出根因复盘（**必读**）|
| [`MD/HANDOFF.md`](MD/HANDOFF.md) | 本次架构重构的交接文档（**已归档**，记的是"重构中踩过什么坑"）|
| [`MD/REBUILD.md`](MD/REBUILD.md) | 本次重构的计划书（**已归档**）|
| [`MD/APIserver.md`](MD/APIserver.md) | ⚠️ **已被 `PROTOCOL.md` 取代**（V1.5，仅供理解历史）|
| [`MD/APP_FSM.md`](MD/APP_FSM.md) | ⚠️ **旧**架构（重构前的 `AppFsm` 状态机），仅供理解历史 |
| [`MD/problem.md`](MD/problem.md) | 历史问题排查记录 |
| [`tools/`](tools/) | 网关协议探针（纯 Node，无依赖）+ TTS 测试语音 |

---
协议（V1.1 长连接）
- [STACK_OVERFLOW.md](STACK_OVERFLOW.md) — 栈溢出根因复盘（必读）
- [problem.md](problem.md) — 历史问题排查记录
- [HANDOFF.md](HANDOFF.md) — 项目交接文档
- [AGENTS.md](AGENTS.md) — 项目开发指南

## License

（待补充）

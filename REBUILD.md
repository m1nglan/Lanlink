# Lanlink 业务调度层重构：语音/UI 事件化 + 去轮询化

## Context（背景与目标）

Lanlink（ESP32-S3, IDF v6.0.2, LVGL 9.5）当前所有业务逻辑缠在 `AppFsm` 状态机里，由 `ws_task` 每轮 `tick()` 轮询驱动，占 CPU 且难扩展。本次重构：

1. **去轮询**：业务改"事件/命令驱动"，不再靠 ws_task 轮询状态机。
2. **协议层/调度层分离**：`RtAsr`/`Llm`/`WS` 是干净的无状态协议接口（几乎不改），重写的是调度层。
3. **语音 → 接口调用**：`voice_start()/voice_stop()` 异步发起，结果经队列回 UI。
4. **输入事件化**：按键改 GPIO 中断 + esp_timer 消抖；编码器消费端并入 LVGL（indev）。
5. **为 chat 屏上屏铺路**：本次搭好双通道队列骨架，UI 只 log 占位，等 SquareLine 画完再接 label。
6. **LLM 链本次不做**，枚举/常量声明式预留。

### 范围边界
- 本次**只搭底层**（队列 + voice API + lv_timer 消费骨架先 log），不接具体 UI 控件。
- IO10/IO8 物理键保留，改中断化；IO8 只留回调钩子（未来功能）。
- LLM 转发链不做；chat 屏文字上屏是后续。
- 保留常驻：ws_keeper（原 ws_task）/ lvgl_task / 新 voice_task。

## 已确认架构决策

| 项 | 结论 |
|---|---|
| voice_task | 常驻、阻塞在 cmd_q；START→采音循环，STOP→发 end→回队列睡。**异步式，无信号量** |
| final 处理 | websocket_task（组件任务）回调自动收 → 投 resp_q → UI 拉。**voice_task 不等 final** |
| 触发源 | UI 中心化：按钮/编码器的事件回调（LVGL 上下文）调 `voice_start()` 投 cmd_q |
| 结果回 UI | **双通道**：resp_q（可靠）+ stream_q（流式覆盖），见下 |
| 编码器 | 保留 ISR+解码；消费端改 LVGL indev read_cb（消灭独立 encoder_task 与跨任务锁） |
| 按键 | GPIO 中断 + esp_timer(~15ms) 消抖 → 按下/释放双沿回调 → 投 cmd_q |
| app_fsm | 删除，LLM 枚举/常量迁 `business/llm_chain.hpp` 声明式预留 |
| 聊天历史 | **环形 20 条**；超限 `lv_obj_del` 最老 panel → 文字内存自动回收 |

## 双通道队列设计（本次核心）

```
resp_q   (普通队列, 深度16)   ← 控制类: ASR final / 状态 / 错误 / 服务切换
                               语义: 每条都不能丢, 逐条处理
                               xQueueSend / xQueueReceive

stream_q (深度1 覆盖队列)      ← 流式: ASR partial 增量文本
                               语义: 只留最新, 旧的随便覆盖, 自动合并
                               xQueueOverwrite / xQueueReceive
```

**为什么双通道**：流式 partial 高频（可能 50Hz），而 UI 每 50ms 才刷一次。若都用普通队列，会：① 生产者（websocket_task）不能阻塞 → 满则丢 → 可能丢 final；② 逐条消费做无用功（第 N 条被第 N+1 条取代）。

**stream_q 的关键语义**（用户确认）：
- 生产者 `xQueueOverwrite`：**永不阻塞、永不失败**，只留最新那句
- 消费者 `xQueueReceive`：**队列非空 = 有变化才刷新**；空则跳过，不 malloc、不重绘
- 这天然实现了"变化检测状态机"（用户原想手写 flag=1/刷完=0），且**无竞态**（FreeRTOS 内部临界区保证），文本随消息原子拷贝、无撕裂

**stream_q 传完整句子**（不传指针）：生产者侧用 `RtAsr` 的累积缓冲拼成完整句，`strlcpy` 进 `m.text`（**绝不 strcpy**，防截断越界）。

## 消息结构（`business/bus_msg.hpp`）

```cpp
typedef enum { CMD_NONE=0, CMD_VOICE_START, CMD_VOICE_STOP,
               CMD_SVC_SWITCH,          // 未来 IO8
               CMD_LLM_CHAT_TEXT,       // 未来
} cmd_t;
typedef struct { cmd_t cmd; int32_t arg; } cmd_msg_t;      // 8B

typedef enum { STREAM_ASR_PARTIAL=0, STREAM_LLM_PARTIAL } stream_kind_t;
typedef struct { stream_kind_t kind; char text[512]; } stream_msg_t;

typedef enum { RESP_ASR_FINAL=0, RESP_ASR_STATUS, RESP_WS_STATUS,
               RESP_LLM_FINAL, RESP_STATUS } resp_kind_t;
typedef struct { resp_kind_t kind; uint32_t flags; uint32_t t_ms;
                 char text[512]; } resp_msg_t;             // 约520B

#define CMD_Q_LEN 4
#define RESP_Q_LEN 16
```
尺寸说明：`text[512]` 按"一句话"容量；final 超长会被 `strlcpy` 截断（可接受）。
内存：resp_q ≈ 8KB，stream_q ≈ 0.5KB，走 RTOS 堆。

## 目标任务布局

| 任务 | 核 | prio | 栈 | 职责 |
|---|---|---|---|---|
| ws_keeper（原 ws_task 改造） | 0 | 5 | 8K | ping 保活 + 断线重连 + 重连后 attach/切 text |
| websocket_task（组件自带） | 0 | 5 | — | 收包 → 同步跑回调 → 投 resp_q/stream_q |
| voice_task（新） | 0 | 6 | 6K | 命令驱动采音/发 audio/end |
| lvgl_task（含 encoder read_cb + ui timer） | 1 | 2 | 6K | 渲染 + 消费 resp_q/stream_q |

## ⚠️ 已亲自验证的 4 个架构硬伤（实现必须遵守）

1. **websocket_task 默认不绑核**（组件 `tskNO_AFFINITY` prio5）；`ws.cpp` 未设 task 参数 → 回调可能跑 CPU1 抢 LVGL。
   → **`ws.cpp::init()` 必须加 `.task_core_id = 0; .task_prio = 5;`**
2. **组件 TX/RX 共锁**：send 走 `tx_lock+lock`，RX 解析持 `lock` 同步跑回调 → **voice_task 必须与 websocket_task 同核(CPU0)**，把锁竞争降为核内短临界区。
3. **LVGL 锁是非递归互斥量**（`xSemaphoreCreateMutex`）：LVGL 上下文内（read_cb / lv_timer，已持锁）**绝不能再调自带 `lvgl_port_lock()` 的函数**（如旧 `lvgl_port_send_encoder_dir`）→ 用**不带锁的内部函数**。
4. **asr/llm 必须全局常驻**（回调 ctx 生命周期=系统）：不能放任务栈/循环里重建。ws handler 表 deinit 不清空、attach 幂等，重连勿"换对象重 attach"。

## 文件清单

### 新建 `main/business/`
| 文件 | 职责 |
|---|---|
| `bus_msg.hpp` | 队列/消息类型（上节） |
| `ui_bridge.hpp/.cpp` | 单例：建 cmd_q/resp_q/stream_q；`init()` 绑 asr 回调投队列；`voice_start()/voice_stop()`；`start_ui_timer()` 注册 lv_timer 消费队列（本次 log）；`report_status()` |
| `voice.hpp/.cpp` | `voice_task`（CPU0 prio6 栈6K）+ 语音会话逻辑 |
| `ws_keeper.hpp/.cpp` | `ws_keeper_task`（CPU0 prio5 栈8K）：保活+重连+attach+切text |
| `llm_chain.hpp` | 仅 `enum llm_stage_t` + 超时常量 + 接线注释（不编译逻辑） |

### 新建 `main/drivers/`
| 文件 | 职责 |
|---|---|
| `button_edge.hpp/.cpp` | GPIO 中断 + esp_timer 消抖 + 按下/释放双沿回调，取代 `button.*` |

### 修改
| 文件 | 改动 |
|---|---|
| `main/drivers/ws.cpp` | init() 加 task_core_id/task_prio；注释"回调全在组件任务上下文" |
| `main/drivers/encoder.hpp/.cpp` | 删 encoder_task/回调；保留 ISR+queue；暴露 `int encoder_consume_raw(void)`（非阻塞返回净跳变） |
| `main/display/lvgl_port.hpp/.cpp` | 加 `lvgl_port_register_encoder_indev()`（**须持锁调用**，read_cb 用**不带锁**的内部读/聚焦函数）；加内部 `lvgl_port_focus_active_screen()` |
| `main/CMakeLists.txt` | SRC_DIRS/INCLUDE_DIRS 加 `"business"`；删 app_fsm 源 |
| `main/main.cpp` | 重写 app_main；删 ENABLE_AUDIO_TASKS/button_task/i2s_task/stream_buffer |

### 删除
`main/drivers/app_fsm.{hpp,cpp}`（留档改名 `app_fsm.cpp.txt` 防 CMake 收集）、`main/drivers/button.{hpp,cpp}`。

### 复用（不动）
`rtasr.*` `llm.*` `ws.*`（除 init 加核）`i2s_mic.*` `wifi.*` `lcd_display.*` `ui.c`。

## voice_task 核心结构

```cpp
static void voice_task(void*) {
    I2sMic mic; ESP_ERROR_CHECK(mic.init());   // 仅建通道
    bool recording = false;
    while (1) {
        cmd_msg_t m;
        if (xQueueReceive(s_cmd_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.cmd==CMD_VOICE_START && !recording) recording = voice_session(mic);
    }
}
static bool voice_session(I2sMic &mic) {
    // 等连接(15s超时) → 清残留STOP → 切text+300ms → mic.start + asr.start
    static int16_t pcm[I2S_MIC_FRAME_BYTES/2];
    while (1) {
        if (mic.read_frame(pcm,sizeof(pcm),40)==ESP_ERR_TIMEOUT) continue;
        if (xQueueReceive(s_cmd_q,&m,0)==pdTRUE && m.cmd==CMD_VOICE_STOP) break;  // 帧尾peek
        if (!ws.is_connected()) { report(VOICE_ABORTED); mic.stop(); asr.end(100); return false; }
        if (asr.send_audio((uint8_t*)pcm,sizeof(pcm),WS_SEND_TIMEOUT_MS)!=ESP_OK){ /*abort*/ }
    }
    asr.end(...); mic.stop(); report(VOICE_STOPPED);
    return true;   // 回 cmd_q 睡, 不等 final
}
```
STOP 最坏延迟 = 帧边界 40ms + 消抖 15ms ≈ <60ms（协议固有，勿切开音频帧）。

## 按键（button_edge）要点

- ANYEDGE 中断 ISR **只做 `esp_timer_restart(tmr, 15000)`**（IRAM 安全）；15ms 无新中断后 timer 回调（esp_timer 任务上下文）读稳定电平，与上次比较，变了触发对应沿。
- 回调在普通任务上下文 → 投 cmd_q 安全（8B 非阻塞）。
- IO10 按下→`CMD_VOICE_START`，释放→`CMD_VOICE_STOP`；IO8 只留钩子 log。

## 聊天历史与上屏（后续阶段，本次留接口）

- **消息列表**：一个指向 panel 的指针数组，最多 20 条；超限时 `lv_obj_del(最老 panel)`，其下 label 文字内存由 LVGL 析构自动释放（[lv_label.c:774](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L774)）。
- **不需要自己开 char 数组存文字**：`lv_label_set_text` 内部已复制一份到 label（[lv_label.c:1009](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L1009)）。
- **流式冒字**：生产者把增量拼成完整句入 stream_q（覆盖式），UI `xQueueReceive` 到才刷；final 到达后生成一条正式消息 panel，清空 stream_q。
- 注意 `lv_label_set_text` **不比较内容**，同内容也会 free+malloc+重绘（[lv_label.c:981](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L981)）→ 必须靠 stream_q 的"非空才刷"避免无谓开销。

## 分阶段实施

### 阶段 0：基础设施与删除
business/ 目录 + bus_msg.hpp + ui_bridge 空壳（建三队列）+ llm_chain.hpp + app_fsm 留档删除 + ws.cpp 补 task 核参数。
**验证**：`idf.py build` 过；串口无 "handler 表已满"。

### 阶段 1：输入中断化 + 编码器进 LVGL
button_edge + 接线（先只 log）；encoder 去 task 化 + lvgl encoder indev。
**验证**：按键一次一沿无抖动；转编码器仍切屏；任务清单无 "encoder"；无 Guru Meditation/死锁。

### 阶段 2：ws_keeper + UiBridge + voice 骨干（先不采音）
ws_keeper 纯保活/重连；UiBridge 收 asr 回调投队列 + lv_timer 消费 log；voice START 暂只"等连接+切text+start→end"。
**验证**：冷启动建连+切text；手动 voice_start/stop 看 start/end 发出无错。

### 阶段 3：voice 真采音闭环（核心）
完整 voice_session（I2S/DMA/每帧send/断线中止）+ stream_q 流式链路。
**验证**：按住 IO10 说话松开 → 终端 RtAsr 流式 partial + final；ui_bridge 打 RESP_ASR_FINAL；快速点按不崩；拔网线录音→VOICE_ABORTED+ws_keeper重连→再按可开新会话；录音中 UI 仍可转编码器不卡。

### 阶段 4：清理加固
删留档/未用代码；i2s_mic static 缓冲唯一消费者注释；调 voice prio/resp_q 深度/ui timer 周期；长期运行 heap 稳定。

---

## 🧑💻 你可以自己写的部分（Arduino 背景友好度）

按"需要碰多少 IDF 特有 API"排序，从最容易上手到最难：

### ✅ 很适合你写（纯逻辑，Arduino 手感）
| 部分 | 为什么适合 | 涉及文件 |
|---|---|---|
| **聊天消息列表管理** | 就是数组增删 + 判满删最老，纯 C 逻辑，无 IDF API | `business/chat_history.cpp`（待建） |
| **上屏逻辑**（后续） | `lv_label_set_text(ui_restext, text)` 这类调用，和 Arduino 的 `lcd.print()` 手感一样 | ui_bridge 消费部分 |
| **UI 事件回调体** | 在 SquareLine 生成的回调里调 `voice_start()`，就是"按键触发→调函数" | ui_chat.c 等（SquareLine 生成） |
| **按键的业务映射** | IO10→录音、IO8→切服务，就是回调里发命令 | button_edge 的 cb 体 |
| **状态显示**（如"录音中…"） | 简单 label 切换 | ui_bridge 消费部分 |

### 🟡 可以写但要我搭好脚手架
| 部分 | 说明 |
|---|---|
| **voice_task 会话逻辑** | 主体是 I2S 读帧循环（类似 Arduino `analogRead` 循环），但 `xQueueReceive`/`I2sMic::read_frame` 是 IDF API，我写好骨架你填业务判断 |
| **stream_q 消费逻辑** | 核心就 `xQueueReceive` + `lv_label_set_text`，我搭好后你改显示逻辑 |

### 🔴 建议我写（IDF 特有、易踩坑）
| 部分 | 为什么 |
|---|---|
| 队列创建/任务创建/绑核 | `xQueueCreate`/`xTaskCreatePinnedToCore` 参数多，且坑 1/2 的核决策在这 |
| esp_timer 消抖中断 | ISR 安全、回调上下文，Arduino 没有对应概念 |
| **LVGL indev 注册** | `lv_indev_create/set_type/set_read_cb` + 非递归锁坑 3，最容易死锁 |
| ws.cpp 补核参数 | 组件内部机制 |
| CMakeLists 改 | IDF 构建系统 |

### 建议的协作方式
1. **我先把阶段 0-1 的骨架全部写好并保证能编译**（队列/任务/中断/indev 这些 IDF 重活）。
2. **你接手写"业务层"**：chat_history 的消息数组管理、上屏逻辑、事件回调体 —— 这些几乎不碰 IDF 怪 API，你能顺手写，而且这部分才是"你自己设备的行为"。
3. 卡住的地方随时问我，我给接口和示例。

## 关键风险提醒
- LVGL 上下文消费端禁用自带锁函数（坑 3）。
- `read_frame` 的 static 缓冲 → 全机唯一采音者（删旧 i2s_task 后 OK）。
- WS send 失败不置 m_connected=false → 用 `is_connected()`+send返回值双判断。
- partial 路由靠 `WS::m_service` → 每次会话前必须 `ws.set_service("text")`。
- cmd/resp 队列别吃内部 SRAM（如需要可改 `xQueueCreateStatic` + PSRAM）。

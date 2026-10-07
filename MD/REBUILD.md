# Lanlink 业务调度层重构：语音/UI 事件化 + 去轮询化

> ## ✅ 当前状态：**阶段 0 / 1 / 2 / 3 已全部完成**，只剩 **阶段 4（清理加固）**
>
> 端到端已跑通：**按住录音键 → 聊天屏立刻冒气泡 → 识别文字逐字上屏 → 松开定格**。
>
> **超出原计划的部分**：下行协议从 V1 换成了 **V2**（见 [`PROTOCOL.md`](PROTOCOL.md)）——
> ASR 改"全量替换"、废除 `revise`、`partial` 归 LLM 独占、`stream_q` 上永远是完整文本。
> 起因是实测踩到"只显示 `结果` 两个字"的坑（根因在网关丢帧 + 板子累积逻辑），详见 `PROTOCOL.md` 开头。
>
> 本文档作为**重构计划书**保留：每个阶段下面都标了"**实际做成了什么样 / 和原计划的差异**"。
> 当前进度与遗留项以 [`HANDOFF.md`](HANDOFF.md) 为准。

## Context（背景与目标）

Lanlink（ESP32-S3, IDF v6.0.2, LVGL 9.5）当前所有业务逻辑缠在 `AppFsm` 状态机里，由 `ws_task` 每轮 `tick()` 轮询驱动，占 CPU 且难扩展。本次重构：

1. **去轮询**：业务改"事件/命令驱动"，不再靠 ws_task 轮询状态机。
2. **协议层/调度层分离**：`RtAsr`/`Llm`/`WS` 是干净的无状态协议接口（几乎不改），重写的是调度层。
3. **语音 → 接口调用**：`voice_start()/voice_stop()` 异步发起，结果经队列回 UI。
4. **输入事件化**：按键改 GPIO 中断 + esp_timer 消抖；编码器消费端并入 LVGL（indev）。
5. **为 chat 屏上屏铺路**：本次搭好双通道队列骨架，UI 只 log 占位，等 SquareLine 画完再接 label。
6. **LLM 链本次不做**，枚举/常量声明式预留。

### 范围边界

> 下面是**当初定的**范围；✅/⬜ 是现在的实际完成情况。

- ✅ ~~本次**只搭底层**（队列 + voice API + lv_timer 消费骨架先 log），不接具体 UI 控件~~
  → **实际上上屏也做了**（阶段 3 part B），气泡结构与实现见"聊天历史与上屏"一节
- ✅ IO10/IO8 物理键保留，改中断化（引脚后来改成 **GPIO2 / GPIO42**）
- ⬜ LLM 转发链**仍未做**（驱动已按 V2 改好，但没实例化；`llm_chain.hpp` 是声明式预留）
- ✅ 保留常驻：`ws_keeper`（原 `ws_task`）/ `lvgl_task` / 新 `voice_task`
- ⬜ **唯一还没做的整块：阶段 4 清理加固**

## 已确认架构决策

| 项 | 结论 |
|---|---|
| voice_task | 常驻、阻塞在 voice_q；START→采音循环，STOP→发 end→回队列睡。**异步式，无信号量** |
| final 处理 | websocket_task（组件任务）回调自动收 → **投 `stream_q`（V2 改的，原来走 `resp_q`）** → UI 拉。**voice_task 不等 final** |
| 触发源 | UI 中心化：物理键的事件回调调 `voice_start()` 投 voice_q<br>（⚠️ 回调实际跑在 **esp_timer 任务**上下文，不是 LVGL 上下文）|
| 结果回 UI | **双通道**：resp_q（可靠）+ stream_q（流式覆盖），见下 |
| 编码器 | 保留 ISR + 查表解码 + 累加器；消费端改 LVGL indev read_cb（消灭独立 encoder_task 与跨任务锁）|
| 按键 | GPIO 中断 + esp_timer(**50ms**）消抖 + 同 tick 限流 → 按下/释放双沿回调 → 投 voice_q |
| app_fsm | 删除，LLM 枚举/常量迁 `business/llm_chain.hpp` 声明式预留 |
| 聊天历史 | `CHAT_MAX_BUBBLES = **10**` 条（原计划 20）；超限 `lv_obj_del` 最老 wrapper → 删父删子，文字内存自动回收 |

## 双通道队列设计（本次核心）

```
resp_q   (普通队列, 深度16)   ← 控制类: 会话状态 / 连接状态 / 短提示
                               语义: 每条都不能丢, 逐条处理
                               xQueueSend / xQueueReceive

stream_q (深度1 覆盖队列)      ← 长文本: 识别结果 / LLM 回复
                               语义: 只留最新, 旧的随便覆盖, 自动合并
                               xQueueOverwrite / xQueueReceive
```

**为什么双通道**：流式文本高频（可能 50Hz），而 UI 每 ~137ms 才刷一次。若都用普通队列，会：① 生产者（websocket_task）不能阻塞 → 满则丢 → 可能丢 final；② 逐条消费做无用功（第 N 条被第 N+1 条取代）。

### V2 实际数值

| 宏 | 值 | 用在哪 | 为什么是这个值 |
|---|---|---|---|
| `BUS_TEXT_LEN` | **2048** | **只用在 `stream_q`** | 深度 1 覆盖式 → **只占一份**，所以开大很便宜（512→2048 只多花 1.5KB）|
| `BUS_TEXT_LEN_SHORT` | **64** | `resp_q` 的 `u.text` | 深 16 → **每 +1 字节就是 ×16**。64 ≈ 21 汉字，够放"服务切换超时"这类带数值的短提示 |
| `VOICE_Q_LEN` / `RESP_Q_LEN` / `STREAM_Q_LEN` | 4 / 16 / 1 | — | ⚠️ 覆盖式队列**必须**是 1，否则 `xQueueOverwrite` 断言 |

**队列总计 ≈ 3304 B 内部 SRAM**（V1 是 8932 B → **反而省了 5.6KB**）：

```
stream_msg_t:     4 + 4 + 2048     = 2056  × 1  =  2056 B
resp_msg_t:       4 + 4 + 4 + 64   =   76  × 16 =  1216 B
voice_cmd_msg_t:  8                        × 4  =    32 B
```

### ★ 一条原则让两条队列同时变省："长文本只走覆盖式队列"

| 队列 | 因为这条原则 |
|---|---|
| `stream_q` | 深度 1 → **敢把 `BUS_TEXT_LEN` 开到 2048**（只占一份）|
| `resp_q` | 长文本搬走后 → `u.text` **才敢从 512 缩到 64**（省 7.2KB）|

**⚠️ 反例**：如果长文本走深 16 的 `resp_q`，`BUS_TEXT_LEN=2048` 就要 `16 × 2048 = 32KB` —— **根本做不到**。

> `resp_q` 里那个 512 是 V1 遗留：当时 `RESP_ASR_FINAL` 走 `resp_q`、完整句子塞在 `u.text` 里。
> V2 之后 final 改走 `stream_q`，`resp_q` 退化成**纯状态通道**，512 就成了空气（每条实际只用 `u.sta` 的 4 字节）。

### stream_q 的关键语义

- 生产者 `xQueueOverwrite`：**永不阻塞、永不失败**，只留最新那句
  ★ 必须在 WS 回调里用 —— 回调持着 `client->lock`，用 `portMAX_DELAY` 会让整个 WS 收发死锁
- 消费者 `xQueueReceive`：**队列非空 = 有变化才刷新**；空则跳过，不 malloc、不重绘
- 这天然实现了"变化检测状态机"，且**无竞态**（FreeRTOS 内部临界区保证），文本随消息原子拷贝、无撕裂

### ★★ stream_q 上的 text 永远是【完整文本】，不是增量

生产者侧必须拼好完整句子再进 `m.text`（`strlcpy`，**绝不 `strcpy`**，防截断越界）。
于是消费者**零累积状态** —— 收到什么就 `lv_label_set_text` 什么。

**两个服务的累积责任分配：**

| 服务 | 外部协议 | 谁做累积 | 送到总线上的 |
|---|---|---|---|
| **text (ASR)** | `{"type":"asr"}` 每次发**完整句** | **网关** | 完整句 → 板子直接整体替换 |
| **llm / openclaw** | `{"type":"partial"}` 发**增量** | **板子的 `Llm` 驱动**（WS 回调里 `strlcat`）| 完整回复 |

**★★ 硬约束（从现有架构推出来的，别违反）**：外部协议若是"发增量"，**累积必须在【发布到总线之前】完成** ——
也就是在 WS 回调里、驱动的 `handle_*` 里，**绝不能放到 UI 侧**：

```
❌ UI 侧累积（错）
   网关 → partial "今" ─┐
   网关 → partial "天" ─┤ 都进 stream_q (深度1, 覆盖式)
   网关 → partial "好" ─┘
                        ↓ UI 每 ~137ms 才取一次
   只取到"好" → "今""天"【永久丢失】 ✗

✅ 驱动侧累积（对）
   WS 回调: s_reply = "今" → "今天" → "今天好"      (每条都同步并进缓冲)
   每次都把 s_reply 投进 stream_q (覆盖式)           ← 被覆盖也无所谓, 已经是全量
   UI 取到的是"今天好" ✓
```

**一句话记牢：累积由"生产侧"做，总线两侧永远只有全量状态。**
（ASR 的"生产侧"是网关，LLM 的"生产侧"是板子的 `Llm` 驱动 —— 位置不同，原则一样。）

## 消息结构（`business/bus_msg.hpp`）

```cpp
// ---- 命令通道 voice_q (普通队列, 深 4) ----
typedef enum { VOICE_NONE=0, VOICE_START, VOICE_STOP,
               VOICE_LLM_CHAT_TEXT,     // 预留: 把文本发给 LLM
               VOICE_SVC_ACKED }        // 服务器确认服务切换完成
             voice_cmd_t;
typedef struct { voice_cmd_t cmd; int32_t arg; } voice_cmd_msg_t;   // 8B
// ★ arg 只放【标量】—— 4 字节装不下文本; 更不要拿它塞指针
//   (那等于把"值拷贝"退回成"指针 + 一块无人保护的内存")

// ---- 流式通道 stream_q (深度 1, 覆盖式) ----
typedef enum { STREAM_ASR=0, STREAM_LLM } stream_kind_t;   // V2: 不再叫 _PARTIAL
#define BUS_TEXT_LEN 2048          // ★ 只用在 stream_q (深 1 → 只占一份)
typedef struct {
    stream_kind_t kind;
    bool          is_final;        // ★ V2 新增: 把"流式"和"完成"合并进一条消息
    char          text[BUS_TEXT_LEN];
} stream_msg_t;                    // ≈ 2056 B

// ---- 控制通道 resp_q (普通队列, 深 16) ----
typedef enum { RESP_WS_STATUS=0,   // 连接状态  → u.sta
               RESP_ASR_STATUS,    // 会话状态  → u.sta
               RESP_STATUS }       // 通用短提示 → u.text
             resp_kind_t;
// ★ V2: RESP_ASR_FINAL / RESP_LLM_FINAL 已删除 —— 改由 stream_msg_t.is_final 表达

// 统一的状态值: 不管哪个服务, 报给 LVGL 的状态都写这里
typedef enum { CONNECTED=0, DISCONNECTED, RECONNECTING,
               STARTED, ENDED, ABORTED } status_kind_t;

#define BUS_TEXT_LEN_SHORT 64
typedef struct { resp_kind_t kind; uint32_t flags; uint32_t t_ms;
                 union { char text[BUS_TEXT_LEN_SHORT]; int32_t i32; float f32;
                         status_kind_t sta; } u;
               } resp_msg_t;                              // 76B (×16 = 1.2KB)

#define VOICE_Q_LEN  4
#define RESP_Q_LEN   16
#define STREAM_Q_LEN 1
```

**★ 核心规则：`stream_q` 上的 `text` 永远是【完整文本】，不是增量。**
→ 消费者**零累积状态**：收到什么就 `lv_label_set_text` 什么，不需要判断"该替换还是该追加"。

**为什么把"完成"塞进 `is_final`、而不是留一条 `RESP_ASR_FINAL`：**
- `is_final` 描述的是"**当前这条文本**是不是最终版" —— 它本来就该跟着文本一起被取代
- 覆盖式队列里，新文本取代旧文本是**正确语义**（新一段开始了，旧的 final 自然作废）
- 于是 `resp_q` 彻底退化成纯状态通道，`u.text` 也才敢缩到 64

> ⚠️ 代价：`final` 从"可靠事件"变成了"最新状态里的一个属性"，理论上会被后一条覆盖。
> **实际不可能**：覆盖需要"final 之后 137ms 内又来一条"，而 final 之后本轮就结束了，下一条得等用户再按键。
> 且"本轮结束"还有一条**可靠**通道：`resp_q` 里的 `RESP_ASR_STATUS` + `ENDED`。

尺寸说明：`BUS_TEXT_LEN`=2048 ≈ 680 汉字（一句话的**极端上限**，实际一句 20~40 字）；
`BUS_TEXT_LEN_SHORT`=64 ≈ 21 汉字（短提示够用）。超长会被 `strlcpy` 截断（可接受）。
内存：三队列共 ≈ **3304 B**，走 RTOS 堆（内部 SRAM）。

## 目标任务布局

| 任务 | 核 | prio | 栈 | 职责 |
|---|---|---|---|---|
| ws_keeper（原 ws_task 改造） | 0 | 5 | 8K | ping 保活 + 断线重连。**不切服务**（归 voice_task：会话开头切 + 等 svc_ok） |
| websocket_task（组件自带） | 0 | 5 | — | 收包 → 同步跑回调 → 投 resp_q/stream_q |
| voice_task（新） | 0 | 6 | 6K | 命令驱动采音/发 audio/end |
| lvgl_task（含 encoder read_cb + ui timer） | 1 | 2 | 6K | 渲染 + 消费 resp_q/stream_q |

## ⚠️ 已亲自验证的 4 个架构硬伤（实现必须遵守）

1. **websocket_task 默认不绑核**（组件 `tskNO_AFFINITY` prio5）；`ws.cpp` 未设 task 参数 → 回调可能跑 CPU1 抢 LVGL。
   → **`ws.cpp::init()` 必须加 `.task_core_id = 0; .task_prio = 5;`**
2. **组件 TX/RX 共锁**：send 走 `tx_lock+lock`，RX 解析持 `lock` 同步跑回调 → **voice_task 必须与 websocket_task 同核(CPU0)**，把锁竞争降为核内短临界区。
3. **LVGL 锁是非递归互斥量**（`xSemaphoreCreateMutex`）：LVGL 上下文内（read_cb / lv_timer，已持锁）**绝不能再调自带 `lvgl_port_lock()` 的函数**（如旧 `lvgl_port_send_encoder_dir`）→ 用**不带锁的内部函数**。
4. **asr/llm 必须全局常驻**（回调 ctx 生命周期=系统）：不能放任务栈/循环里重建。ws handler 表 deinit 不清空、attach 幂等，重连勿"换对象重 attach"。

## 文件清单（✅ 已对照实际代码核实）

### 新建 `main/business/`
| 文件 | 职责 |
|---|---|
| `bus_msg.hpp` | **纯类型**：三队列消息定义（见上文）。不含逻辑 |
| `ui_bridge.hpp/.cpp` | 单例。建三队列；`init()` 绑 asr / svc_ok / error 回调；`voice_start()/voice_stop()`；`start_ui_timer()` 注册 lv_timer；**`drain_queues()` 消费队列 → 聊天视图**（`chat_add_me_bubble` / `chat_view_show_asr` / `chat_view_begin_asr` / `chat_view_end_asr` / `chat_view_sync_screen`）|
| `voice.hpp/.cpp` | `voice_task`（CPU0 prio6 栈6K）+ `voice_session`（真采音闭环）|
| `ws_keeper.hpp/.cpp` | `ws_keeper_task`（CPU0 prio5 栈8K）：保活 + 重连 + 状态上报。**不切服务** |
| `llm_chain.hpp` | 仅 `enum llm_stage_t` + 超时常量 + 接线注释（**声明式预留，不编译逻辑**）|

### 新建 `main/drivers/`
| 文件 | 职责 |
|---|---|
| `button_edge.hpp/.cpp` | GPIO 中断 + `esp_timer` 消抖 + 按下/释放双沿回调（取代 `button.*`）|
| `gpio_isr_once.hpp` | `gpio_isr_service_ensure()` —— ISR 服务幂等安装（encoder 与 button_edge 共用）|

### 修改
| 文件 | 改动 |
|---|---|
| `main/drivers/ws.hpp/.cpp` | init() 加 `task_core_id/task_prio`；新增 `set_svc_ok_callback()`；新增 `ws_timeout_ticks()`（修超时单位）；`dispatch_msg` 的 `error` 分支不再早退；**删掉 `set_partial_handler` 那套"按服务分流"**（V2 下 `asr` 只属 text、`partial` 只属 llm，一维 `type` 就能区分）；`handle_data` 改成**在 `m_rx_buf` 里就地分发**（省掉 1KB 栈拷贝、也不再截断长消息）|
| `main/drivers/encoder.hpp/.cpp` | 删 encoder_task / 回调；保留 ISR + 累加器；暴露 `int encoder_consume_raw(void)`（非阻塞返回净跳变）|
| `main/drivers/rtasr.hpp/.cpp` | **V2 重写**：注册 `"asr"`/`"final"`；`store_and_notify()` **一律 `strlcpy` 整体替换**（不再 `strlcat`）；**删掉 `handle_revise`/`revise`**；去掉 WS 回调里的 `printf` |
| `main/drivers/llm.hpp/.cpp` | **V2**：注册 `"partial"`/`"reply"`；**新增 `set_stream_callback()`**（签名与 `rtasr_result_cb_t` 同形状）；`handle_partial` **在驱动侧累积**后回调"完整回复" |
| `main/drivers/i2s_mic.hpp/.cpp` | 新增 `static int frame_level()`（一帧平均绝对幅度 0~32767，用于音量日志）|
| `main/display/lvgl_port.hpp/.cpp` | 加 `lvgl_port_register_encoder_indev()`（**须持锁调用**；read_cb 用**不带锁**的内部函数）；加 `lvgl_port_focus_active_screen_locked()` |
| `main/CMakeLists.txt` | SRC_DIRS / INCLUDE_DIRS 加 `"business"`；删 app_fsm 源 |
| `main/main.cpp` | 重写 app_main；删 ENABLE_AUDIO_TASKS / button_task / i2s_task / stream_buffer |
| `main/lvgl/screens/ui_chat.c` | **（人改的）**：加 `merollpanel` / `resrollpane` 两层 wrapper；`ui_contextpanel` 开滚动；`ui_TabView3` 加 `LV_OBJ_FLAG_HIDDEN`；`ui_respanel` 补底色 |

### 删除（改名留档，不参与编译）
`main/drivers/app_fsm.{hpp,cpp}.txt`、`main/drivers/button.{hpp,cpp}.txt`
（⚠️ 改名成 `.txt` 是为了让 `file(GLOB)` 的 `SRC_DIRS` 收不到它们）

### 复用（确实没动）
`wifi.*`、`lcd_display.*`、`ui.c`（主题初始化）、`fonts/*`（除下面的字体坑）

### 文档
| 文件 | 说明 |
|---|---|
| `MD/PROTOCOL.md` | **新** —— V2 协议规范（板子↔网关 + 内部总线），取代 `MD/APIserver.md` 的 V1.5 |
| `MD/APIserver.md` | ⚠️ 顶部已加"**已被 PROTOCOL.md 取代**"的说明。它同时是**网关侧的接口文档**，改动要转给网关作者 |
| `MD/REBUILD.md` | 本重构计划书（本文件）|
| `MD/HANDOFF.md` | 当前状态 / 正在排查的问题 / 源码级验证过的坑 —— **接手项目先读它** |

## voice_task 核心结构（✅ 已是实际代码的简化版）

```cpp
static void voice_task(void *arg) {
    I2sMic &mic = ...;
    mic.init();                        // ★ 只做一次 (不是每轮建通道)
    while (1) {
        voice_cmd_msg_t m;
        if (xQueueReceive(voice_q, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.cmd == VOICE_START) voice_session(ub, mic);
        // VOICE_STOP / VOICE_SVC_ACKED 在空闲时只记日志 (会话里才处理)
    }
}

static void voice_session(UiBridge &ub, I2sMic &mic) {
    ub.post_resp_status(RESP_ASR_STATUS, STARTED);   // ★ 按下就上报 → UI 立刻冒空气泡
    // ① 等连接 (15s) —— ★ 循环里必须 voice_q_drain(), 否则队列满了连 STOP 都丢
    //      (实测踩过 `voice_q 满, VOICE_STOP 丢弃` → 用户松手被忽略)
    // ② 清残留命令 → ws.set_service("text") + asr.switch_service("text")  【必须成对】
    // ③ wait_svc_ok(300ms)   ← 真等服务器确认, 不再盲目 vTaskDelay
    // ④ asr.start() → mic.start()   ★ mic 必须在 asr.start 之后开, 否则前几帧白发
    static int16_t pcm[I2S_MIC_FRAME_BYTES/2];
    uint32_t t_rec = now_ms();
    while (1) {
        esp_err_t r = mic.read_frame(pcm, sizeof(pcm), I2S_MIC_FRAME_MS);
        // ★ 边界②: 不管读没读到, 都先看 STOP / 单轮上限 / 断线
        if (xQueueReceive(voice_q, &m, 0) == pdTRUE && m.cmd == VOICE_STOP) break;
        if (now_ms() - t_rec >= VOICE_MAX_RECORD_MS) break;          // 50s 上限 → 正常收尾
        if (!ws.is_connected()) { aborted = true; break; }
        // ★ 边界①: 读失败时 pcm 还是上一帧旧数据 → 绝不能发
        if (r != ESP_OK) { read_fail++; continue; }
        if (asr.send_audio((uint8_t*)pcm, sizeof(pcm), WS_SEND_TIMEOUT_MS) != ESP_OK) { aborted = true; break; }
        frames++;
    }
    mic.stop();                        // ★ 所有退出路径都必须执行
    asr.end(WS_SEND_TIMEOUT_MS);
    ub.post_resp_status(RESP_ASR_STATUS, aborted ? ABORTED : ENDED);
    // ★ 不等 final —— websocket_task 收到后经 asr 回调投 stream_q, 由 UI 消费
}
```

- **STOP 最坏延迟** = 帧边界 40ms + 消抖 **50ms** ≈ <90ms（协议固有，勿切开音频帧）
- ⚠️ **`read_frame` 失败时的 `continue` 等于扔掉这 40ms 音频** → 所以必须有 `read_fail` 计数（见阶段 3 基准表）

## 按键（button_edge）要点

- ANYEDGE 中断 ISR **只做两件事**：① 同 tick 限流（`xTaskGetTickCountFromISR`）② `esp_timer_restart(tmr, 50000)`（IRAM 安全）
- **50ms 无新中断**后 timer 回调（esp_timer 任务上下文）读稳定电平，与上次比较，变了触发对应沿
- 回调在普通任务上下文 → 投 voice_q 安全（8B 非阻塞）
- **录音键按下 → `VOICE_START`，释放 → `VOICE_STOP`**；另一个键只留钩子 log
  （⚠️ 引脚后来改过：录音键 IO10→**GPIO2**、服务键 IO8→**GPIO42**，见 `main.cpp`）
- ⚠️ **ISR 里别做重活**：`esp_timer_restart` 会屏蔽中断 + 两次有序链表遍历；按键抖动时反复重入
  会让 CPU0 泡在 ISR 里 → **中断看门狗 panic**（实测踩过，backtrace 就停在本 ISR 里）。限流后最多 100 次/秒
- ⚠️ **`gpio_config()` 之后不能立刻采初值**：引脚还没稳，会把状态记错 → 之后误报一次边沿。
  实测踩过"开机 2.4 秒、用户没碰按键，冒出 `[按键] 录音键 松开`"。
  修法：**连采到"连续 10 次不变"（最多 100ms）再定初值**

## 聊天历史与上屏（✅ 已完成 —— 原本列在"后续阶段"）

### 控件结构（三层）

```
ui_contextpanel  (flex column, 可滚动)                     ← SquareLine 画的容器
└── roll   (wrapper: 310 宽 × SIZE_CONTENT 高, 透明)        ← flex 的孩子
    └── panel (气泡: SIZE_CONTENT, align=TOP_RIGHT, 绿底 0x35D28D)
        └── label (文字: SIZE_CONTENT, max_width 280, 字体 ui_font_ch14)
```

**每新建一条消息就是复制这一整套**（`ui_bridge.cpp::chat_add_me_bubble`）。

### ★ 为什么需要 `roll` 这一层 wrapper

**LVGL 的 flex 不支持"每条单独对齐"** —— `cross_place` 是**整容器一个值**，所有孩子一样，
做不到"我的靠右、对方的靠左"。

wrapper 破解法：
- **`roll` 是 flex 的孩子** → 被 flex 按列排下来，它自己的 `x`/`y`/`align` **全部无效**
- **`panel` 是 `roll` 的孩子，而 `roll` 没有 layout** → **`panel` 的 `align` 生效** ✓

源码依据（`lv_obj_pos.c:777`）：
```c
void lv_obj_refr_pos(lv_obj_t * obj)
{
    if(lv_obj_is_layout_positioned(obj)) return;   // ★ 父对象有 layout → 直接 return
    ...
    lv_align_t align = lv_obj_get_style_align(obj, ...);   // ← align 只有到这里才被用
}
```
**一句话规则：一个对象的 `x`/`y`/`align` 是否生效，取决于【它的父对象】有没有 layout。**

| 对象 | 父 | layout? | x/y/align |
|---|---|---|---|
| `ui_contextpanel` | `ui_chat`（屏幕）| 无 | ✅ 生效 |
| `roll`（wrapper）| `ui_contextpanel` | **flex** | ❌ 无效 |
| `panel`（气泡）| `roll` | 无 | ✅ **生效** |
| `label` | `panel` | 无 | ✅ 生效（但靠 padding 摆，不设 x/y）|

### ★ 尺寸链（全靠 `LV_SIZE_CONTENT` 自动收敛）

```
文字 + 字体度量          → label 的 SIZE_CONTENT 宽高
   ↓ + padding (8/8/6/6)
label 实际尺寸           → panel 的 SIZE_CONTENT
   ↓ align TOP_RIGHT
panel 尺寸               → roll 的 SIZE_CONTENT 高
   ↓ flex column
ui_contextpanel 依次往下堆（间距靠容器的 pad_row=5）
```

**⚠️ 第 3 步能成立，是因为 LVGL 认 `TOP_RIGHT` 的"垂直分量"。** 依据（`lv_obj_pos.c:1492`）：
`LV_ALIGN_TOP_RIGHT` 被归入 **"Normal top aligns"**，于是父对象的高度会算上孩子的高度。
（当初若用 `LV_ALIGN_CENTER`，会走 `default` 分支，结果就不一样。）

### ★ 气泡生命周期由【会话状态】驱动（不是由文字）

| 时机 | 触发 | 动作 |
|---|---|---|
| **按下录音键** | `RESP_ASR_STATUS` + **`STARTED`** | **立刻建一个空气泡**（`chat_view_begin_asr`）|
| 松开 / 中止 | `RESP_ASR_STATUS` + `ENDED`/`ABORTED` | 收尾（`chat_view_end_asr`）|

**为什么不等有文字再建**：从"按下"到"服务器回第一个字"要经过
**等连接 → 切服务 → 等 svc_ok → start → 用户开口 → 讯飞识别**，可能一两秒。
这段时间屏幕毫无反应，用户会以为坏了。**先把气泡画出来 = 即时的"收到按键了"反馈。**

**⚠️ 按了键但一个字都没出**（没说话 / 服务器没回）→ 结束时检查 `lv_label_get_text()` 是否为空串，
是就把这个空气泡删掉（否则屏幕上留一个莫名其妙的小绿块）。

### ★ 屏幕生命周期：按需创建 + 离开即销毁

```c
// ui_chat.c:40
lv_obj_add_event_cb(ui_chat, scr_unloaded_delete_cb, LV_EVENT_SCREEN_UNLOADED, ui_chat_screen_destroy);
```

**离开聊天屏 → 整屏被 `lv_obj_del` → `ui_chat`/`ui_contextpanel` 全部置 NULL。**
**⚠️ 于是记在 `s_bubbles[]` / `s_live_label` 里的指针全部变成【悬空指针】——
下一次 `lv_obj_del(s_bubbles[0])` 就是 use-after-free。**

两道保险（`chat_view_sync_screen`，每轮 `drain_queues` 都调）：
1. **换屏检测**：`s_ctx_hooked != ui_contextpanel` → 清空所有记录 + 顺手删掉 SquareLine 预置的占位气泡
2. **有效性检查**：`!lv_obj_is_valid(s_bubbles[0])` → 整体作废

**`lv_obj_is_valid()`（`lv_obj.c:450`）遍历所有屏幕的对象树、只做【指针比较】、不解引用入参
→ 对悬空指针也是安全的** —— 这是它最宝贵的一点。

**⚠️ 另一个必须知道的坑：`lv_obj_create(NULL)` 会创建一个【新屏幕】，不是子对象！**
→ 所以建气泡前**必须**先确认 `ui_contextpanel != NULL`。

### 内存与生命周期

- **删父删子**：`lv_obj_del(roll)` 连带删掉 `panel` 和 `label`（LVGL 是树形结构）
- **★ 不需要自己开 char 数组存文字**：`lv_label_set_text` 内部已复制一份到 label
  （[lv_label.c:1009](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L1009)），
  删 label 时由 LVGL 析构自动释放
  （[lv_label.c:774](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L774)）
- **超限淘汰最老的**：`CHAT_MAX_BUBBLES = 10`；满了 `lv_obj_del(s_bubbles[0])` + 数组左移
- 新建后 `lv_obj_update_layout()` + `lv_obj_scroll_to_view(roll, LV_ANIM_ON)` 滚到可见处
  （`lv_obj_update_layout` 在 lv_timer 回调里安全：重入时直接 early return，`lv_obj_pos.c:383`）
- 注意 `lv_label_set_text` **不比较内容**，同内容也会 free+malloc+重绘
  （[lv_label.c:981](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L981)）
  → 靠 `stream_q` 的"非空才刷"避免无谓开销 ✓ **（原文这条写对了，保留）**

### 💤 文字放 PSRAM（已实现，但**当前用开关关着**）

**动机**：`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` → **小于 16KB 的 `malloc` 全落内部 SRAM**。
而 LVGL 用的是 CLIB malloc（`CONFIG_LV_USE_CLIB_MALLOC=y`）→ 走 `lv_label_set_text` 的文字**永远进不了 PSRAM**。

**做法**：`heap_caps_malloc(n, MALLOC_CAP_SPIRAM)` 自己分配 + `lv_label_set_text_static()` 只存指针
（static 模式下 [lv_label.c:774](D:/Desktop/ESP-IDF/Lanlink/managed_components/lvgl__lvgl/src/widgets/label/lv_label.c#L774)
的 `if(!label->static_txt)` 保证 LVGL 不会去 free 我们的指针）+ `LV_EVENT_DELETE` 回调负责 `heap_caps_free`。

**当前 `#define CHAT_TEXT_IN_PSRAM (0)` 关着。**
> ★ 当时开了之后"文字不显示"，一度以为这条路径坏了 —— **其实是下面的字体坑，跟它无关。**
>
> **内存账（为什么现在关着也没事）**：一条识别结果通常 20~40 字 = 60~120 字节，
> 10 条 ≈ 1~2KB 内部 SRAM，可忽略。
> **真正需要它的是 LLM 长回复**（每条可能上千字节 × 10 条 = 10~20KB）—— 到那时再打开。

### ⚠️⚠️ 踩过的字体坑（务必记住，排查花了很久）

**症状**：气泡**框的长度对，但一个字都不画**（连 SquareLine 预置的"你好"/"hello"也不显示）。

**根因**：`ui_font_ch14.c` 的 `.bitmap_format = 1`（= `LV_FONT_FMT_TXT_COMPRESSED`），
而 `CONFIG_LV_USE_FONT_COMPRESSED` 没开：

```c
// lv_font_fmt_txt.c:207
else {
#if LV_USE_FONT_COMPRESSED
    decompress(...);
#else
    LV_LOG_WARN("Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled");
    return NULL;          // ★ 一个像素都不画
#endif
}
```

**为什么表现为"框对、字没有"**：取宽度走 `lv_font_get_glyph_dsc()`（**不看 `bitmap_format`**），
取像素走 `lv_font_get_glyph_bitmap()`（**看**）—— 一个正常、一个返回 NULL。
**→ "尺寸对、字不画" 就是这个组合的唯一特征。**

**其它 7 个项目字体（balance22/date22/icon18/time40/time64/updown10/weather18）都是 `.bitmap_format = 0` 明文，
所以只有用 `ui_font_ch14` 的聊天屏中招。**

**修法（当前用的是 ①）**：
① `CONFIG_LV_USE_FONT_COMPRESSED=y`（已写进 `sdkconfig` + `sdkconfig.defaults`，两处都要，原因见下）
② 在 SquareLine 重新导出该字体、**取消勾选 Compress** → `.bitmap_format = 0`，不需要解压器、渲染更快

> ⚠️ **为什么 `sdkconfig` 和 `sdkconfig.defaults` 都要改**：`sdkconfig.defaults` **只在生成新 `sdkconfig` 时**被应用；
> 已有 `sdkconfig` 时必须直接改它才对本次构建生效。
>
> ⚠️ **那个字体文件头写的 `Opts: ... --no-compress --no-prefilter` 是假的（实际压缩了），
> 不要拿它当依据 —— 以 `.bitmap_format` 的值为准。**

## 分阶段实施

### 阶段 0：基础设施与删除 —— ✅ 已完成
business/ 目录 + bus_msg.hpp + ui_bridge 空壳（建三队列）+ llm_chain.hpp + app_fsm 留档删除 + ws.cpp 补 task 核参数。
**验证**：`idf.py build` 过；串口无 "handler 表已满"。 **—— ✅ 已通过**

### 阶段 1：输入中断化 + 编码器进 LVGL —— ✅ 已完成
button_edge + 接线；encoder 去 task 化 + lvgl encoder indev。
**验证**：按键一次一沿无抖动；转编码器仍切屏；任务清单无 "encoder"；无 Guru Meditation/死锁。 **—— ✅ 已通过**

> **实际做成的 / 和原计划的差异：**
> - **消抖从 15ms 提到 50ms** —— 15ms 不够，实测一次按下报多条
> - **修了 3 个坑**：① `lv_group_focus_obj()` 会清掉编辑模式（`lv_group.c:242`）→ 编码器从"旋转"退化成"移动焦点"，
>   故加 `lvgl_focus_active_screen_locked()`；② `HZ=100` 时 `pdMS_TO_TICKS(5)==0` → `vTaskDelay(0)` 不让出 CPU
>   → Task WDT 饿死 IDLE1，加了 tick 夹紧；③ 重复安装 GPIO ISR 服务报错 → `gpio_isr_once.hpp` 幂等
> - **ISR 里加了同 tick 限流** —— `esp_timer_restart` 从 ISR 调**不轻**（屏蔽中断 + 两次有序链表遍历），
>   按键抖动时反复重入会把 CPU0 泡在 ISR 里 → 中断看门狗 panic。限流后最多 100 次/秒

### 阶段 2：ws_keeper + UiBridge + voice 骨干（先不采音）—— ✅ 已完成
ws_keeper 纯保活/重连；UiBridge 收 asr 回调投队列 + lv_timer 消费 log；voice START 暂只"等连接+切text+start→end"。
**验证**：冷启动建连+切 text；手动 voice_start/stop 看 start/end 发出无错。 **—— ✅ 已通过**

> **实际做成的 / 和原计划的差异：**
> - **切服务只归 `voice_task`**，且用 `svc_ok` 回调**真等**服务器确认，不再盲目 `vTaskDelay(300)`
> - **修了 WS 发送超时的单位 bug**：`esp_websocket_client_send_*` 的 `timeout` 参数是 **RTOS ticks 不是 ms**
>   （`esp_websocket_client.h:279`）→ `WS_SEND_TIMEOUT_MS=1000` 实际是 **10 秒**，
>   音频发一帧最多阻塞 10 秒而 DMA 只有 128ms → 整段音频全丢。已加 `ws_timeout_ticks()` 换算

### 阶段 3：voice 真采音闭环（核心）—— ✅ 已完成

**原计划**：完整 voice_session（I2S/DMA/每帧 send/断线中止）+ stream_q 流式链路。

**实际做成的（part A：真采音）**
- `mic.init()` **只做一次**（voice_task 开头），不再每轮建通道
- 采音循环：`mic.read_frame()` → 查 STOP → 查单轮上限 → 查连接 → `asr.send_audio()` 一帧一发
- **`VOICE_MAX_RECORD_MS = 50000` 单轮上限**：网关规定最长 55 秒（`APIserver.md §2`），留 5 秒余量。
  到点走**正常收尾**（`mic.stop` + `asr.end` + `ENDED`），**不是 abort** —— 这样服务器的 final 还能回来
- 连接断开 → `ABORTED`
- **`voice_q_drain()`**：**所有等待循环都必须消费 voice_q** —— 否则"等连接"那最多 15 秒里
  深度 4 的队列会满，连 `VOICE_STOP` 都被丢掉（实测踩过 `voice_q 满, VOICE_STOP 丢弃`）
- 帧数/丢帧/读耗时三个计数器（见下面基准）

**★ 超出原计划的部分：下行协议从 V1 换成了 V2**（见 [`PROTOCOL.md`](PROTOCOL.md)）
- ASR 改「**全量替换**」（网关每次发完整句），**废除 `revise`**，`partial` 归 LLM 独占
- `stream_q` 上的 text 永远是完整文本 → 消费者**零累积状态**
- 起因：实测"只显示 `结果` 两个字"（网关丢了 `rg=[10,N]`/`[17,N]` 的替换帧 + 板子用片段覆盖了累积好的整句）

**part B 上屏** ✅ —— 原本列在"后续阶段"，本次一起做完了（见上一节）

**实测基准（健康值 —— 出问题先对照这张表）**
```
voice: level=xxx | 本秒 发送=25 读失败=0 帧 (满帧应为 25), 读耗时均=34~37ms | GPIO中断累计=1
voice: 会话结束 (正常), 共发 284 帧 (=11.4 秒音频), 读失败丢弃 0 帧 (=0.0 秒)
```

| 指标 | 健康值 | 判读 |
|---|---|---|
| 发送帧/秒 | **25~26** | 满帧（1000/40=25）。**明显偏少 = 正在积压丢音频** |
| 读失败 | **0** | 每多 1 就是 **40ms 音频被扔掉** |
| 读耗时均 | **34~37ms** | 说明 `send_audio` 只花 3~6ms、**余量充足**。若 ≈0 说明发送吃掉了整个 40ms、一点抖动就积压 |
| GPIO中断累计 | 按一次涨**几次~几十** | 涨几百/几千 = 引脚在噪声里翻转（会引发 ISR 风暴 → 中断看门狗 panic）|

> ⚠️ **别漏了这条**：DMA 积压时 `read_frame` **依然会成功**（返回缓冲里最老的数据）
> → "发送帧数正常"**不能单独证明**没丢音频，**必须配合"读耗时"一起看**。

**验证项**：
- ✅ 按住 IO10 说话松开 → 气泡逐字上屏 → 定格
- ✅ 快速点按不崩（`voice_q_drain` 发现 STOP 就取消本轮）
- ⬜ 拔网线录音 → `ABORTED` + `ws_keeper` 重连 → 再按可开新会话
- ⬜ 录音中转编码器是否卡（见阶段 4）

### 阶段 4：清理加固 —— ⬜ **唯一剩下的阶段**

**原计划保留的**
- 删留档 / 未用代码（`app_fsm.*.txt` / `button.*.txt`）
- `i2s_mic` static 缓冲"唯一消费者"注释
- 调 `voice` prio / `resp_q` 深度 / ui timer 周期
- 长期运行 heap 稳定

**✅ 已顺手做掉的**（原本列在阶段 4，实现时一起做了）

| 项 | 说明 |
|---|---|
| **55 秒上限** | `VOICE_MAX_RECORD_MS=50000`，到点**正常收尾**（不走 abort，让 final 还能回来）|
| **`read_frame` 丢帧可见** | 原先读失败是**静默 `continue`**（等于悄悄扔掉 40ms 音频）→ 现在有计数 + 日志 |
| **`error` 处理** | `ws.cpp` 的 `error` 分支不再 `return`，继续走查表分发 → `UiBridge::on_ws_error` 解析 `code`，**1(讯飞错误) / 3(超时) 投 `VOICE_STOP` 让本轮收尾**，不再白发音频 |
| **去掉 WS 回调里的 `printf`** | 持 `client->lock` 打串口既拖住 `send_audio`、又把 UART 中断打爆（那次 Interrupt WDT 的 `EPC1` 就是 `uart_hal_write_txfifo`）→ 降级 `ESP_LOGD` |
| **单轮上限/会话取消的竞态** | `on_ws_error` 投的 STOP 若残留在队列里，会被下一轮的 `voice_q_drain` 当成"用户松手"而取消 → 两道防线：`m_asr_active` 守卫 + `voice_start()` 先清队列 |

**⬜ 待实测**（阶段 3 留下的验证项）
- 拔网线录音 → `ABORTED` + `ws_keeper` 重连 → **再按可开新会话**
- 录音中**转编码器是否卡**（UI 与采音分属两核，预期不卡，要实测）

**⬜ 文档债**
- `HANDOFF.md` / `REBUILD.md` 的 V2 同步（**正在做**）
- **`post_resp()` 目前没有调用者** —— 它是"往 `resp_q` 发短文本（`RESP_STATUS`）"的唯一入口，8 行。
  要么接上一个真实用途（比如把"服务切换超时"这种带数值的提示发到 UI），要么删掉

**⬜ 功能**
- **`Llm` 链路接通**：驱动已按 V2 改好（`set_stream_callback` + **驱动侧累积**），但**还没被实例化**；
  `llm_chain.hpp` 是声明式预留
- **打开 `CHAT_TEXT_IN_PSRAM`**：等 LLM 长回复上来（每条可上千字节 × 10 条 = 10~20KB）再打开

**⬜ 性能 / 加固**
- `xQueueCreateStatic` + PSRAM —— **优先级已降低**：三个队列现在只占 **3304 B** 内部 SRAM
- **渲染性能**：现在 ≈137ms/轮 ≈ **7 FPS**；上屏后要实测
  （`lv_label_set_text` 不比较内容，靠 `stream_q` 的"非空才刷"兜着）
- **C3 剩余**：音频发送超时 → 改成"丢帧继续"而非中止会话（实测**还没触发过**，先不动）

**⬜ 要问网关作者的**
- `asr` 的完整句是**整轮连续累计**，还是每个讯飞段（`rlt`）重新开始？
  （若是后者，`final` 之后的那条 `asr` 会把前面几段顶掉）

---

## 🧑💻 你可以自己写的部分（Arduino 背景友好度）

按"需要碰多少 IDF 特有 API"排序，从最容易上手到最难：

### ✅ 很适合你写（纯逻辑，Arduino 手感）
| 部分 | 为什么适合 | 涉及文件 |
|---|---|---|
| **聊天消息列表管理** | 就是数组增删 + 判满删最老，纯 C 逻辑，无 IDF API<br>**✅ 已完成**（做法：`s_bubbles[]` 存 wrapper 指针，`CHAT_MAX_BUBBLES=10`，满了 `lv_obj_del(s_bubbles[0])` + 数组左移）| `business/ui_bridge.cpp`（原本计划单独建 `chat_history.cpp`，实际就近放这儿了）|
| **上屏逻辑** | `lv_label_set_text(label, text)` 这类调用，和 Arduino 的 `lcd.print()` 手感一样<br>**✅ 已完成** | `ui_bridge.cpp` 的 `chat_view_show_asr()` |
| **UI 事件回调体** | 在 SquareLine 生成的回调里调 `voice_start()`，就是"按键触发→调函数" | ui_chat.c 等（SquareLine 生成）|
| **按键的业务映射** | 按下→录音、释放→停，就是回调里发命令 | `button_edge` 的 cb 体（在 `main.cpp`）|
| **状态显示**（如"录音中…"） | 简单 label 切换<br>⬜ 待做（现在只有气泡，没有状态栏）| `ui_bridge.cpp` 消费部分 |

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

- **LVGL 上下文消费端禁用自带锁函数**（坑 3）—— `lv_timer` 回调里已经持锁，再调 `lvgl_port_lock()` 会自死锁。
- `read_frame` 的 static 缓冲 → **全机唯一采音者**（删旧 i2s_task 后 OK）。
- **WS send 失败不置 `m_connected=false`** → 用 `is_connected()` + send 返回值**双判断**。
- ⚠️ **WS 回调里绝对不能阻塞**：回调持着 `client->lock`（TX/RX **共用一把锁**），
  用 `portMAX_DELAY` 入队会让整个 WS 收发死锁 → `stream_q` 用 `xQueueOverwrite`、`resp_q` 用 `xQueueSend(..., 0)`。
  同理**别在回调里做耗时 `printf`**（会把 UART 中断打爆）。
- ⚠️ **`esp_websocket_client_send_*` 的 `timeout` 参数是 RTOS ticks，不是 ms**
  （`esp_websocket_client.h:279`）→ 必须走 `ws_timeout_ticks()` 换算。
- ✅ **V2 之后 `partial` 不再靠 `WS::m_service` 路由**（`asr` 只属 text、`partial` 只属 llm，一维 `type` 就够）。
  但 `ws.set_service("text")` + `asr.switch_service("text")` **仍必须成对** —— 保持本地视图与服务器一致，便于诊断。
- ⚠️ **`stream_q` 上的 text 必须是【完整文本】**，否则消费者要维护累积状态；
  **累积只能在"发布到总线之前"做**（ASR 在网关做、LLM 在 `Llm` 驱动做，见前文）。
- ⚠️ **`lv_obj_create(NULL)` 会创建一个【屏幕】**，不是子对象 → 建气泡前先确认父指针非 NULL。
- ⚠️ **SquareLine 的屏幕是"离开即销毁"的** → 记下的控件指针会变悬空，
  用之前要过 `lv_obj_is_valid()`（只比指针、不解引用，对悬空指针安全）。
- **队列现在只占 3304 B 内部 SRAM** → `xQueueCreateStatic` + PSRAM **已不是优先项**（见阶段 4）。

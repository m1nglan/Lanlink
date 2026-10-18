# 完整链路（架构重构收尾版）

> 本文只描述**本次重构之后**的架构。它是这次重构的最终产物说明，
> 后续分支的入口文档改为仓库根目录的 [`README.md`](../README.md)。
>
> 阅读顺序建议：先看 §1 总图 → 再看你关心的那条链 → 最后看 §4 的职责边界和铁律。

---

## 1. 总图

```
【输入】                                     【输出】
 按键 GPIO 中断 ─┐                        ┌─→ 绿泡/灰泡 (LVGL)
 编码器 GPIO中断 ─┤  任意上下文, 只投递    │
 I2S DMA ────────┘        │               │
                          ▼               │
        ┌─────────────────────────────────────────────┐
        │  UiBridge  —— 三条队列 + 回调接线            │
        │   voice_q(4) / resp_q(16) / stream_q(1)     │  ← 只搬运, 不做决策
        └───┬──────────────────┬──────────────────┬───┘
            │                  │                  │
   voice_q  │          resp_q  │          stream_q│
            ▼                  ▼                  ▼
     ┌────────────┐    ┌───────────────┐   ┌────────────────┐
     │ voice_task │    │ drain_queues  │   │  drain_queues  │
     │ 真采音      │    │ (状态分派)     │   │  (内容上屏)     │
     └─────┬──────┘    └──┬─────────┬──┘   └───┬────────┬───┘
           │              │         │          │        │
           │        chat_view   ask_router  chat_view  chat_view
           │        (气泡生死)   (何时问谁)   .show_asr  .show_llm
           ▼
      I2S + RtAsr + WS  ──→ 网关

  WS 回调 (websocket_task) ──→ UiBridge::on_* ──→ 投递/转发
```

---

## 2. 五条链

### 2-A 语音上行（按下 → 发音频）

```
① button_edge_isr            (中断)        双边沿
② button_edge_timer_cb       (esp_timer)   50ms 消抖
③ main.cpp::on_button_edge   (esp_timer)   按下/松开 → voice_start/stop
④ UiBridge::voice_start/stop (任意)        投 voice_q
⑤ voice_task::voice_session  (CPU0,6)      等连接→切 text→等 svc_ok→start→采音
⑥ I2sMic::read_frame + RtAsr::send_audio → WS::send_bin → 网关
```

**收尾**：`mic.stop()` → `RtAsr::end()` → `post_resp_status(ENDED)`

### 2-B 识别结果下行（网关 → 屏幕）

```
① websocket_task → WS::ws_event_handler → WS::handle_data   (累积缓冲 + 切分 JSON)
② WS::dispatch_msg                                          (按 type 查表分发)
③ RtAsr::handle_asr / handle_final → store_and_notify
④ [回调] UiBridge::on_asr_result                            (WS 回调, 持 client->lock)
⑤ post_stream(STREAM_ASR, 完整句, is_final)                 (覆盖式, 永不阻塞)
⑥ drain_queues → chat_view::show_asr                        (LVGL 任务)
⑦ chat_view 内部: push/label_set → 气泡
```

**关键**：`stream_q` 里的 text **永远是完整句**（V2 协议）→ 消费者**零累积状态**。

### 2-C LLM 转发（我说完了 → 问 openclaw → 灰泡）

```
① final 到达 → UiBridge::on_asr_result → ask_router::on_asr(text, true)
② ask_router: 有 final 就用 final 立刻问; 没等到就 2.5s 后拿最后一条 asr 兜底
③ UiBridge::ar_switch_service → Llm::switch_service → 网关 {"type":"svc","service":"openclaw"}
④ 网关回 svc_ok → WS → UiBridge::on_svc_ok → ask_router::handle_svc_ok
⑤ UiBridge::ar_send_chat → 网关 {"type":"chat","content":"[Lanlink] <识别文字>"}
⑥ 网关回 partial×N (增量) → Llm 驱动【在驱动侧累积】→ 回调
⑦ UiBridge::on_llm_stream → post_stream(STREAM_LLM, 完整回复, is_final)
⑧ drain_queues → chat_view::show_llm → 灰泡 (灰底/靠左)
```

**占位**：步骤 ⑤ 成功后台立刻投一条 `"(思考中)"` 到 `stream_q`；真回复一到，
`show_llm` 直接**覆盖**它的文字 —— **所以不需要任何"删除占位"的逻辑**。

### 2-D 会话状态（管气泡生死，与 final 无关）

```
按住 → voice_session → post_resp_status(STARTED) → resp_q
        → drain_queues → chat_view::begin_asr()      ← 立刻出【空气泡】
松开 → post_resp_status(ENDED)  → chat_view::end_asr()   ← 只记时刻
        ↓ 2.5 秒宽限期 (CHAT_END_GRACE_MS)
     chat_view::tick() → close_bubble()               ← 空的删掉, 非空的定格
```

**⚠️ 为什么宽限期必须是 2.5 秒**：`end` → `final` 实测 **1470ms**。
早先定 1500ms 时余量只剩 30ms，网络一抖 `final` 就晚于宽限期到达而被丢弃，
屏幕停在最后一条 `asr` 上（差个句号）—— 而日志里 `final` 明明到了，极难排查。

### 2-E 网关通知（讯飞那边先关会话）

```
讯飞长时间无有效语音 → 自行关闭识别会话（单次上传超 50 秒是讯飞硬要求）
        ↓
网关 → {"type":"abroad","text":"<已识别内容>"} 或 {"type":"abroad","text":null}
        ↓
UiBridge::on_abroad:
   有内容 → post_stream(STREAM_ABROAD, text, true)   ← 先上屏（可能一整段）
   然后   → voice_stop()                             ← ★ 无条件立刻收尾
```

**★ 一律立刻结束本轮**，不管 `text` 是什么、也不管用户松没松开 ——
会话已经死了，继续按着说的每一句都进不了识别。

**⚠️ 上屏必须走 `stream_q`**：`resp_q` 的 `u.text` 只有 **64 字节**（≈21 汉字），
一整段话会被截掉大半。

### 2-F 连接保活

```
ws_keeper_task (CPU0,5) → WS::is_connected / is_stale / reconnect
                        → post_resp_status(RESP_WS_STATUS, …) → resp_q
```

**铁律**：`init` / `deinit` / 重连**由 `ws_keeper` 独占**。
业务方自己调 `WS::init()` 会破坏重连逻辑。

---

## 3. 队列

| 队列 | 深度 | 语义 | 走什么 | 消费者 |
|---|---|---|---|---|
| **`voice_q`** | 4 | 命令, 不能丢 | `voice_cmd_t` (START/STOP/SVC_ACKED) | **只有 `voice_task`** |
| **`resp_q`** | 16 | 可靠, 逐条处理 | 状态 + **短**文本 (64B) | `drain_queues` |
| **`stream_q`** | **1** | **覆盖式**, 只留最新 | 长文本 (2048B) | `drain_queues` |

**总计 3304 字节内部 SRAM**（V1 是 8932）。

**三条铁律：**
1. **WS 回调里只能非阻塞投递** —— 那时正持 `client->lock`，阻塞会拖死整个收发。
   `timeout` 一律给 0（`xQueueSend` 的 timeout=0 / `xQueueOverwrite` 永不阻塞）。
2. **值拷贝原则** —— 队列靠 `memcpy` 搬字节，**载荷里绝不能有指针**。
   拷贝在**写者自己的上下文**里做。
3. **`stream_q` 深度必须是 1** —— 长文本只占一份，`BUS_TEXT_LEN` 才敢开到 2048。

---

## 4. 模块职责边界（**每块只做一件事**）

| 模块 | 职责 | **不该做什么** |
|---|---|---|
| `business/bus_msg.hpp` | 纯类型定义 | 不含逻辑 |
| **`business/ui_bridge`** | 队列 + 回调接线 + 会话控制 + 消费分发 | **不做显示, 不做转发决策** |
| **`business/ask_router`** | "何时把哪段文本发给谁" | **不认识 LVGL、不认识队列**（靠注入的回调）|
| **`display/chat_view`** | 气泡创建/更新/销毁 + 生命周期状态机 | **不认识业务**（不知道 ASR/LLM/openclaw）|
| `business/voice` | 语音会话（切服务→start→采音→end）| 不做 UI |
| `business/ws_keeper` | 连接保活与重连 | **不碰语音** |
| `drivers/ws` | 收发 + 按 type 分发 | 不认业务 |
| `drivers/rtasr` | text 服务的协议（`asr`/`final`）| 不做显示 |
| `drivers/llm` | 对话服务的协议（`partial`/`reply`）+ **增量累积** | 不做显示 |

---

## 5. 全局约束

| 约束 | 说明 |
|---|---|
| **LVGL 非线程安全** | 非 LVGL 任务访问 UI 必须 `lvgl_port_lock()`；**但 LVGL 上下文内（回调/lv_timer）绝不能再加锁**（锁非递归，会自死锁）|
| **`drain_queues` 里状态先于内容** | 先处理 `resp_q`（状态）再处理 `stream_q`（内容）。否则 `STARTED` 和第一条 `asr` 同轮到达时，文字先被处理而气泡还没建 → 被当成"已收尾"丢掉 |
| **聊天屏：开机就建，且不销毁** | 当前 SquareLine 导出在 `ui_init()` 里就调 `ui_chat_screen_init()`（`ui.c`），且所有屏幕都**没有** `scr_unloaded_delete_cb` → `ui_contextPanel` 从开机起存在、指针永不改变。<br>⇒ **气泡一直活着**，换屏回来消息还在，不需要重放；`chat_view` 的环形缓冲退化为保险（详见 `chat_view.cpp` 里 `ring_replay` 的注释）。<br>⚠️ 也正因为面板一直在，`show_asr` / `show_llm` **不区分在不在聊天屏** —— 在别的屏时收到消息也照常建气泡（看不见，回到聊天屏就在）。 |
| **`pdMS_TO_TICKS(1..9) == 0`** | `CONFIG_FREERTOS_HZ=100` → 1 tick = 10ms。需要短超时要**夹到 ≥1 tick** |
| **`esp_websocket_client_send_*` 的 timeout 是 RTOS ticks** | 不是毫秒。传错会让一帧音频阻塞 10 秒，而 I2S DMA 只有 128ms |
| **新增源文件后必须 `touch main/CMakeLists.txt`** | `SRC_DIRS` 是 `file(GLOB)`，只在 CMake 配置阶段扫描；不 touch 会报 `undefined reference` |

---

## 6. 一次完整的语音问答（时序）

```
 t=0ms    按下录音键
 t=0ms    [中断] button_edge_isr → 50ms 消抖
 t=0ms    voice_start → voice_q
 t=10ms   voice_task 起来: 等连接 → 切 text → 等 svc_ok(实测 50ms)
 t=60ms   asr.start + mic.start
 t=60ms   ★ 绿泡出现 (空气泡)          ← STARTED 走 resp_q, 立刻
 t=60ms~  asr 逐字增长 → 绿泡逐字刷新
 t=3.9s   松开 → mic.stop → asr.end → ENDED
 t=5.4s   final 到达 (end + 1470ms) → 绿泡显示最终版(带标点)
          同时 ask_router 立刻转发 → svc openclaw (31ms) → chat "[Lanlink] …"
 t=5.4s   ★ 灰泡 "(思考中)" 出现        ← 占位, 因为明岚要 20 秒才回
 t=5.4s   ★ 2.5s 宽限期到 → 绿泡定格
 t=26s    明岚 reply 到 → 灰泡文字被覆盖(占位消失)
```

**✅ 阶段 3 实测全绿**：`end`/`final` 时序、气泡生命周期、编码器锁、拔网线恢复。
**⛔ 测不了**：按住超 50 秒 —— 讯飞硬要求，由 `abroad` 接住。

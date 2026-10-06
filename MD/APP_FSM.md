# Lanlink 状态机工作逻辑详解

> 对应代码: `main/drivers/app_fsm.hpp` / `app_fsm.cpp` + `ws.cpp` / `rtasr.cpp` / `llm.cpp`
> 状态机推进者: `ws_task` 主循环里反复调 `AppFsm::tick()`（非阻塞步进）

## 一、任务与调用关系总览

```
CPU0  ws_task(prio5)      while(1)  s_fsm.tick()      ← 核心推进者,所有状态机在这里步进
CPU1  button_task(prio5)  IO10 长按/松开 → 改 asr_state + (保留录音)
CPU1  i2s_task(prio5)     录音中 → 采 PCM → 写 m_audio_buf   (当前被关:ENABLE_AUDIO_TASKS=0)
CPU1  lvgl_task(prio2)    LVGL UI
```

**共享对象**: `AppFsm s_fsm` 一个全局单例，三个任务各自只碰它的一部分：
- button_task 写 `m_asr_state`(RECORDING/WAITING)，读 `get_state()/is_llm_busy()`
- i2s_task 写音频 StreamBuffer
- ws_task 的 `tick()` 读 `m_asr_state`、推进录音/LLM 两个状态机、收 WS 事件回调改标志

**AppFsm 内部三套独立状态**：

| 状态机 | 取值 | 谁驱动 |
|---|---|---|
| 录音 asr_state | IDLE / RECORDING / WAITING | button_task 置位, tick 消费 |
| LLM 转发 llm_stage | IDLE/SWITCHING/CHATTING/BACK | tick 自推进 + WS 回调 |
| 服务选择 m_llm_service | "openclaw" / "llm" | IO8 按键 → tick 里切换 |

底层一条 **WS 长连接**（`WS` 单例），三个业务层 RtAsr/Llm/AppFsm 都往它 `set_handler/send_text`，服务器消息按 `type` 分发。

---

## 二、tick() 每一步（ws_task 每轮顺序执行）

`tick()` 一次调用走完下面 0→5，然后下一轮再来。**非阻塞**，每步只干"当前该干的最小动作"，大量靠标志位分多轮完成。

### 步骤 0 — 连接保证
```
WS 未连接 或 is_stale(超130s无数据)？
├─ 是: ws.deinit() → 清 m_started/m_end_sent → Reset音频缓冲
│      vTaskDelay(1000ms) → ws.init() 重连
│      成功 → asr.attach(重绑handler) → 发 svc 切到 "text"
│      return ← 本轮到此为止,不往下走业务
└─ 否: 继续
```
- 这是长连接守护：死连接自动重建，重建后强制回 text 服务（服务器要求）。
- 重连时**所有状态保留在内存**，但 `m_started/m_end_sent` 清掉避免发残留。

### 步骤 0.5 — 应用层保活 ping
```
距上次 ping ≥ 20s? → ws.send_ping({type:ping})
```

### 步骤 0.6 — IO8 服务切换（挂起式）
```
m_svc_switch_pending?
├─ llm_stage != IDLE → 拒绝:打印"等回IDLE",保持pending(下轮再试)
├─ WS 未连接 → 拒绝,保持pending
└─ 可切: openclaw⇄llm → ws.set_service + llm.switch_service → 清pending
```
- 关键约束：**LLM 会话进行中禁止切换**（`LLM_IDLE` 才行），避免打断对话。

### 步骤 1 — 录音开始（发 start）
```
asr_state==RECORDING && !m_started ?
├─ ws.set_service("text") + asr.switch_service("text")  ← 再次确保 text
│  vTaskDelay(300ms)  ← 等服务器 svc_ok
│  成功发 {type:start} → m_started=true, m_end_sent=false
│  失败 → delay 50ms 下轮重试
```

### 步骤 2 — 录音中发音频
```
m_started && RECORDING ?
├─ 从 m_audio_buf 取 1280B(20ms超时)
└─ 取到 → asr.send_audio(二进制帧)
```
- i2s_task 每秒 ~15.6 帧(16kHz/1280B)往里写，这里 ~20ms 轮询取。取出超时会自动让出 CPU。

### 步骤 3 — 录音结束（排空+发 end）
```
m_started && WAITING && !m_end_sent ?
├─ 音频缓冲已空 → 发 {type:end} → m_end_sent=true, 记 m_end_time_ms
└─ 还有音频 → 继续取出来发完(先发干净再end)
```
- 这是「松开按键不等于立刻发 end」，先把 i2s 已采未发的音频全部推上去。

### 步骤 4 — final 后复位本轮标志
```
m_started && asr_state==IDLE ?   (IDLE 由 WS回调 on_result 置)
├─ m_started=false, m_end_sent=false
```
- 服务器发 final → `RtAsr::accumulate(is_final=true)` → 触发 `AppFsm::on_result` → 回 IDLE + 存 final_text + 置 `m_llm_pending`。tick 下次进来清掉 start/end 标志。

### 步骤 4.5 — LLM 转发阶段机 ★核心
收到语音 final 后**自动把文本发给当前选中服务**（openclaw/llm），四阶段：

```
[IDLE]  m_llm_pending && WS连着?
  └─ → SWITCHING, 记时, 置 m_llm_busy=true(锁录音)
        reset_stream() + ws.set_service(m_llm_service)
        + llm.switch_service(m_llm_service)

[SWITCHING]  ≥300ms?
  └─ → 发 llm.chat(m_final_text) → 清pending → CHATTING, 记时
        (300ms 是等 svc 切换完成的固定窗口, 不依赖 svc_ok 消息)

[CHATTING]   收到 reply?  → LLM_BACK, 记时
        或 超 120s?       → LLM_BACK (打超时警告)

[BACK]   llm.switch_service("text") + ws.set_service("text")
         → IDLE, m_llm_busy=false
         清 m_end_sent/m_end_time_ms ← 防录音残留
```
- **为什么回 text**：识别(text)和对话(llm)都走同一条 WS，发完 LLM 必须切回 text，否则下轮语音的 partial/start 服务器不处理。
- **m_llm_busy 全程 true** → 期间 button_task 拒绝开始新录音。
- LLM 回复流式 partial 打到串口；完整 reply 时 `handle_reply` → `on_llm_reply` 置 `m_reply_received`。

### 步骤 4.6 — WAITING 超时兜底
```
llm_stage==IDLE && asr_state==WAITING && end已发 && 超10s没等到final?
└─ → 强回 IDLE, 清标志
```
- 防服务器丢了 final 时永远卡在 WAITING（此时若 LLM 正忙则不兜底，让位给 LLM）。

### 步骤 5 — 让出 CPU
```
asr_state != RECORDING → vTaskDelay(20ms)
（录音中靠步骤2的 xStreamBufferReceive 20ms超时让出）
```

---

## 三、WS 回调链（异步,由 esp_websocket_client 事件触发）

收到的每条消息 `WS::dispatch_msg` 解析 JSON → 按 `type` 找 handler：

| type | 处理器 | 动作 |
|---|---|---|
| partial + 当前服务=text | RtAsr::handle_partial | 增量**追加** buffer，打印新增字 |
| partial + 当前服务=llm | Llm::handle_partial | 增量**追加**对话流 buffer，逐字打印 |
| final | RtAsr::handle_final | **覆盖** buffer → `on_result`(回IDLE+转发) |
| revise | RtAsr::handle_revise | **整体替换** buffer（修正错字），不回IDLE |
| reply | Llm::handle_reply | 覆盖对话 buffer → `on_llm_reply`(置 reply_received) |
| (svc_ok/pong 等) | — | 无专门handler,静默 |

**partial 路由靠 `WS::m_service`**：同一 type=partial 消息，text 服务时进 ASR、llm 服务时进 LLM——所以 AppFsm 每次切服务都同步调 `ws.set_service()` 保持两边一致。

---

## 四、端到端一个完整会话（按住 IO10 说话→LLM 回复）

```
1 按住IO10 500ms → button_task 确认 → s_fsm.begin_recording()   [IDLE→RECORDING]
2 tick步骤1 → svc text + 发 start → m_started
3 tick步骤2 ⇄ i2s_task 并行：你说话,音频持续上行
4 松开IO10 → button_task → end_recording()                       [RECORDING→WAITING]
5 tick步骤3 → 排空缓冲 → 发 end → 记 m_end_time
6 服务器回 partial/final... revise... final
   → RtAsr覆盖buffer → on_result → [WAITING→IDLE] 存final_text + m_llm_pending
7 tick步骤4 → 清 start/end 标志
8 tick步骤4.5: IDLE→SWITCHING(锁录音,切到openclaw)→300ms后 chat
   → CHATTING(等回复) → 收到 reply → LLM_BACK → 切回text → IDLE,解锁录音
9 期间想换服务: 按IO8 → pending → 等llm_stage回IDLE才真正切
```

---

## 五、当前代码里两处"可能要注意"的逻辑点（供参考,未改）

1. **步骤1 用固定 300ms 等 svc 切换**，不解析 svc_ok；步骤4.5 SWITCHING 也是固定 300ms。若服务器切换慢会偶发 start 早于 svc_ok。
2. **WAITING 超时兜底被 `llm_stage != IDLE` 跳过**——若 LLM 回复期间麦克风 WAITING 超时，不会兜底，靠后面 LLM 回 text 时清残留，机制上依赖了 4.5 的 BACK 清理。

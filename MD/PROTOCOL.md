# LanLink 协议规范（V2）

> 取代 [`APIserver.md`](APIserver.md) 的 V1.5。
> **改动原因**：V1.5 的"ASR 走增量"方案在板子侧引入了**跨消息累积状态**，
> 由此衍生出一整类难查的 bug（实测踩到的）：
>
> | 实测现象 | 根因 |
> |---|---|
> | 只显示 `"结果"`，前面全丢 | 网关的 `revise` 发的是**片段**，板子按"整体替换"用它覆盖了累积好的整句 |
> | 屏幕只更新一次 | 网关只发了 2 条 `partial`，**大量丢弃**讯飞的 `rg=[10,N]`/`[17,N]` 帧 |
> | `final` 只有两个字 | 网关的整句维护逻辑坏了，输出的就是片段 |
>
> **V2 的核心思想：让"流式文本"永远是【完整的当前状态】，把累积的责任收到一个明确的地方。**

---

## 1. 设计原则

| # | 原则 | 理由 |
|---|---|---|
| **1** | **每个服务的流式类型，语义要么"全量"、要么"增量"，绝不混用** | V1.5 的 `partial` 被 text 和 llm 共用，还要按 `m_service` 分流，语义一乱就出上面那些 bug |
| **2** | **板子内部总线上的"流式文本"永远是【完整文本】** | 消费者（UI）**不需要任何累积状态** → `lv_label_set_text(s.text)` 一行搞定 |
| **3** | **累积只在一个地方发生，而且必须在"发布到总线之前"完成** | 否则覆盖式队列（深度 1）会在消费者慢时**丢掉增量** |
| **4** | **长文本只走一条覆盖式队列** | 覆盖式只占一份，所以 `BUS_TEXT_LEN` 可以开大（2048）；可靠队列的深度不会被长文本吃掉 |
| **5** | **不引入"需要跨消息累积才能理解"的协议** | 每条消息自解释。丢一条、乱序、重复，都不影响最终结果 |

---

## 2. 外部协议（网关 ↔ 板子）

### 2.1 服务与消息一览

| 服务 | 上行（板子→网关） | 下行·流式 | 下行·完成 |
|---|---|---|---|
| **text**（语音听写） | `start` → PCM → `end` | **`asr`（全量）** | `final`（全量） |
| **llm / openclaw**（对话） | `chat` / `clear` | **`partial`（增量）** | `reply`（全量） |
| **usage / weather** | `get` | `usage` / `weather`（自带全量数据） | — |
| **echo** | 任意 | `echo` | — |
| 通用 | `svc` | `svc_ok` | — |
| 通用 | `ping` | `pong` | — |
| 通用 | — | `error` | — |

### 2.2 text（语音听写）★ **本次改动最大**

```
板子 → {"type":"start"}                        开始
板子 → [PCM 二进制帧]  16kHz/16bit/mono, 1280B/帧 ≈ 40ms
网关 → {"type":"asr","text":"<完整当前句>"}      ★ 每次都是【完整句】，板子直接替换
       ...（可多条，每条都是"此刻的完整句"）
板子 → {"type":"end"}                          结束
网关 → {"type":"final","text":"<完整最终句>"}    ★ 完整句 + 本轮完成
```

**要点：**

| 项 | 规定 |
|---|---|
| **`asr` 的 `text`** | **必须是"从头到现在的完整句"**，不是新增的字 |
| **`asr` 的频率** | 讯飞每次中间结果都可以发；建议合并到 **≤10 条/秒**（这个量对板子毫无压力，实测一条 ≈100µs）|
| **修正** | **`asr` 本身就携带修正后的全量** → **不再需要 `revise`**（V1.5 的 `revise` 已废除）|
| **`final` 的 `text`** | **完整句**（不是片段、不是最后一小段）|
| **`final` 的时机** | 收到板子的 `end` 之后 |
| 单轮上限 | 55 秒（超出回 `error code=3`）|
| `start` 前发音频 | 丢弃 |

**板子侧行为**：`strlcpy(buf, text, sizeof(buf))` —— **一条赋值，零状态**。

### 2.3 llm / openclaw（对话）★ **保持 V1.5**

```
板子 → {"type":"chat","content":"记作业：数学第3页"}
网关 → {"type":"partial","text":"<新增字>"}     增量（多条，板子累积）
       ...
网关 → {"type":"reply","content":"<完整回复>"}  完整回复 + 本轪完成
板子 → {"type":"clear"}                        清空上下文
```

**要点：**

| 项 | 规定 |
|---|---|
| **`partial` 的 `text`** | **只含新增的字**（后缀）|
| **`reply` 的 `content`** | **完整回复** |
| 工具调用空窗 | 明岚调工具期间可能几秒~几十秒无输出，板子无需感知，等 `reply` 即可 |
| **`partial` 归属** | **从此只属于 llm/openclaw**（text 服务不再使用）|

**⚠️ 板子侧的关键设计**：LLM 的增量**累积发生在板子的 `Llm` 驱动里**（WS 回调上下文中），
**发布到内部总线时已经是"完整回复"**。理由见 §4。

### 2.4 报文格式约定

- 所有下行都是**单条完整 JSON 对象**（网关自己决定分帧）
- 文本字段的 JSON 转义必须正确（`"` `\` 换行等）
- **不要求** `seq`/`id`/时间戳 —— 板子不依赖顺序（因为流式文本是全量）

### 2.5 与 V1.5 的差异总表

| 消息 | V1.5 | **V2** |
|---|---|---|
| `partial`（text 服务）| **增量**，板子 `buffer += text` | ❌ **废除** —— 改用 `asr` |
| **`asr`** | 不存在 | ✅ **新增**：完整当前句，全量替换 |
| `partial`（llm/openclaw）| 增量 | ✅ **不变** |
| `revise` | "整体替换" | ❌ **废除** —— `asr` 已含修正 |
| `final` | "完整" | ✅ **不变，但要求真的是完整句** |
| `reply` | 完整回复 | ✅ **不变** |

---

## 3. 内部总线（板子内）

### 3.1 两条通道

| 队列 | 深度 | 语义 | 装什么 |
|---|---|---|---|
| **`stream_q`** | **1（覆盖式）** | 只留最新 | **长文本**：`STREAM_ASR` / `STREAM_LLM`，**永远是完整文本** |
| **`resp_q`** | 16（可靠） | 每条都要处理 | **短消息**：连接状态 / 会话状态 / 通用提示 |
| `voice_q` | 4 | 命令 | （不变）|

### 3.2 类型定义

```c
/*! 长文本容量。ASR 一句 / LLM 一段回复都放得下。
 *  ★ 只用在 stream_q 上（深度 1，覆盖式）—— 所以开大只花一份内存。 */
#define BUS_TEXT_LEN  2048      /* 512 → 2048 */

/* ---------------- 流式通道（覆盖式，深度 1）---------------- */
typedef enum {
    STREAM_ASR = 0,     /*!< 语音识别: 完整当前句 */
    STREAM_LLM,         /*!< 大模型: 完整回复 (Llm 驱动侧已累积好) */
} stream_kind_t;

typedef struct {
    stream_kind_t kind;
    bool          is_final;         /*!< ★ 新增: true = 这句是最终版, UI 可以"定格" */
    char          text[BUS_TEXT_LEN];
} stream_msg_t;                     /*!< 约 2056 字节 */

/* ---------------- 控制通道（可靠, 深度 16）---------------- */
typedef enum {
    RESP_WS_STATUS = 0,     /*!< 连接状态   → u.sta */
    RESP_ASR_STATUS,        /*!< 会话状态   → u.sta */
    RESP_STATUS,            /*!< 通用提示   → u.text (短) */
} resp_kind_t;

typedef enum {              /*!< 统一状态值 (不变) */
    CONNECTED = 0, DISCONNECTED, RECONNECTING, STARTED, ENDED, ABORTED,
} status_kind_t;

typedef struct {
    resp_kind_t kind;
    uint32_t    flags;
    uint32_t    t_ms;
    union {
        char          text[BUS_TEXT_LEN_SHORT];   /* 64, 只用于短提示 */
        int32_t       i32;
        float         f32;
        status_kind_t sta;
    } u;
} resp_msg_t;               /*!< 76 字节 (V1 是 524 —— u.text 512 缩到 64) */
```

**`RESP_ASR_FINAL` / `RESP_LLM_FINAL` 退休** —— "完成"由 `stream_msg_t.is_final` 表达。

### 3.3 内存账

```
stream_msg_t:     4 + 4 + 2048     = 2056  × 1  =  2056 B
resp_msg_t:       4 + 4 + 4 + 64   =   76  × 16 =  1216 B
voice_cmd_msg_t:  8                        × 4  =    32 B
────────────────────────────────────────────────────────
                            合计 ≈ 3304 B 内部 SRAM
                     (V1 是 8932 B → **−5.6 KB**)
```

**为什么总量反而变小了：`resp_q` 里的 `u.text` 从 512 缩到 64。**

> V1 时代 `RESP_ASR_FINAL` 走 `resp_q`，完整句子塞在 `u.text` 里 → 需要 512 字节。
> V2 之后识别结果/LLM 回复全归 `stream_q`（深 1），`resp_q` 退化成**纯状态通道**，
> 那个 512 就成了空气：`512 × 16 = 8384 B` 白占，而每条实际只用 `u.sta` 的 4 字节。
> 缩到 64（≈21 汉字，够放"服务切换超时"这类提示）后 `resp_q` 从 8384 掉到 1216。
>
> **这就是"长文本只走覆盖式队列"这条原则的额外收益** —— 它同时让两条队列都变省了。

**代价可接受，换来"消费者零状态"。**

---

## 4. 谁做累积（**这是整个设计的核心**）

| 服务 | 累积在哪 | 送到总线上的 |
|---|---|---|
| **text** | **网关** | 完整句 → 板子直接 `strlcpy` ✓ |
| **llm / openclaw** | **板子的 `Llm` 驱动**（WS 回调里 `strlcat` 到 `s_reply`）| 完整回复 ✓ |

**为什么 LLM 的累积必须在**发布到总线之前**（也就是在 WS 回调里）完成：**

```
❌ 如果让 UI 侧累积:
   网关 → partial "今" ─┐
   网关 → partial "天" ─┤ 都进 stream_q (深度 1, 覆盖式)
   网关 → partial "好" ─┘
                        ↓
   UI 每 ~137ms 才取一次 → 只取到"好" → "今""天" 永久丢失 ✗✗✗

✅ 在驱动侧累积:
   WS 回调: s_reply = "今" → "今天" → "今天好"    (每次都同步完成)
   每次都把 s_reply 投进 stream_q (覆盖式)         ← 被覆盖也无所谓, 因为已经是全量
   UI 取到的是"今天好" ✓✓✓
```

**这条规则对 ASR 同样成立** —— 所以网关发全量，本质上是**把累积也放在"发布之前"**（网关那一侧）。

---

## 5. 板子侧改动清单

| # | 文件 | 改动 |
|---|---|---|
| 1 | `business/bus_msg.hpp` | `BUS_TEXT_LEN` 512→2048；`stream_msg_t` 加 `is_final`；`stream_kind_t` 改名 `STREAM_ASR`/`STREAM_LLM`；`resp_kind_t` 去掉 `RESP_ASR_FINAL`/`RESP_LLM_FINAL`；**新增 `BUS_TEXT_LEN_SHORT`=64**（V1 的 512 是留给 `resp_q` 装完整句的，V2 不用了）|
| 2 | `drivers/rtasr.cpp` | `accumulate()` 一律 `strlcpy`（不再 `strlcat`）；**删掉 `revise()` 和 `handle_revise()`**；`handle_partial` 改名 `handle_asr`；**注册 `"asr"`**（不再注册 `"partial"`/`"revise"`）|
| 3 | `drivers/rtasr.hpp` | 对应改名 |
| 4 | `drivers/llm.cpp` | `handle_partial` **累积到 `s_reply`**，再把 **`s_reply`（全量）** 投总线；`chat()` 时清 `s_reply`；`handle_reply` 覆盖 `s_reply` 并标 `is_final` |
| 5 | `business/ui_bridge.cpp` | `on_asr_result` → 拆成 `on_asr_text(text, is_final)` / `on_llm_text(text, is_final)`，都走 `post_stream(kind, text, is_final)`；`post_stream` 多一个 `is_final` 参数；`post_resp` 里的 `RESP_ASR_FINAL` 分支删掉 |
| 6 | `business/voice.cpp` | 会话结束报 `RESP_ASR_STATUS/ENDED`（已经是这样）✓ |
| 7 | `business/ws_keeper.cpp` | 不变 ✓ |
| 8 | `drivers/ws.cpp` | `dispatch_msg`：`partial` **不再按服务分流**（它只属于 LLM）→ 去掉那个特判；新增 **`asr` 走普通查表** |
| 9 | `business/ui_bridge.cpp` `drain_queues` | 消费者逻辑（阶段 3 上屏）：`stream_q` → `lv_label_set_text`，`is_final` 时定格。**零累积状态** |

### 5.1 `ws.cpp::dispatch_msg` 的简化

```c
/* V1.5: partial 被 text 和 llm 共用 → 必须按 m_service 分流 (特判)
 * V2:   asr     → 只属于 text  → 普通查表 ✓
 *       partial → 只属于 llm   → 普通查表 ✓
 *       于是那个按 m_service 的特判可以【整体删掉】 */
```

---

## 6. 网关侧改动清单

| # | 改动 |
|---|---|
| 1 | **修 `rg` 处理**（当前 bug）：`pgs=="rpl"` 且 `rg[0] > 1` 的帧**必须应用**（替换 `words[rg[0]..rg[1]]`），**不能"局部替换帧忽略"** |
| 2 | **维护一个 `words[]` 词数组**：`apd` 追加 / `rpl` 替换范围 / `rlt` 段完结 |
| 3 | **`asr` 下发**：每次拼出 `words[]` 的**完整句** |
| 4 | **删掉 `revise` 下发**（已废除） |
| 5 | **`final` 下发完整句**（不是片段） |
| 6 | `partial` / `reply`（LLM）**保持现状** ✓ |
| 7 | 更新 `APIserver.md` |

### 6.1 `asr` 拼句的正确做法（可直接参考）

讯飞 IAT 返回：

```json
{"data":{"result":{
   "ws":  [{"cw":[{"w":"开"}]}, {"cw":[{"w":"始"}]}, ...],  // 本次涉及的词
   "rg":  [10, 12],          // 本次【变化的词序号范围】
   "pgs": "rpl" | "apd",     // rpl=替换范围内内容, apd=追加到末尾
   "rst": "pgs" | "rlt"      // pgs=中间结果, rlt=一段完结
}}}
```

```python
if pgs == "apd":
    words.extend(ws_words)              # 追加
else:  # "rpl"
    words[rg[0]:rg[1]+1] = ws_words     # ★ 替换范围 (当前这里被整帧丢弃了)
sentence = "".join(words)               # 完整句
send({"type": "asr", "text": sentence})
```

---

## 7. 迁移与验证

### 7.1 迁移顺序

```
① 网关先改（asr 能发完整句 + 修 rg）      ← 必须先做
② 板子再改（总线 + 驱动 + 消费者）
③ 两边一起烧，验证
```

**⚠️ 不能反序**：网关还在发增量、板子已经改成覆盖 → 屏幕只会显示最后一个增量（更糟）。

### 7.2 验证点

| # | 看什么 | 期望 |
|---|---|---|
| 1 | 启动日志 `ui_bridge: 就绪: voice_q=4 resp_q=16 stream_q=1 (共约 3304 字节…)` | **比 V1 的 8932 还小** ✓ |
| 2 | 说话时 `[流式] kind=0 final=0 text="..."` | **text 每次都是完整句，逐步变长** |
| 3 | 松开后 `[流式] kind=0 final=1 text="..."` | **完整句 + final=1** |
| 4 | 屏幕 | 逐句冒字 → 定格 |
| 5 | `level=` 行 | `发送=25 读失败=0`（音频链路不受影响）|
| 6 | 问 LLM 一句 | `[流式] kind=1` 的 text **逐步变长**（驱动侧已累积），最后 `final=1` |

### 7.3 回滚

**改动集中在 3 个文件**（`bus_msg.hpp` / `rtasr.{hpp,cpp}` / `ui_bridge.cpp` + `llm.cpp`），
**队列数量、任务布局、`voice_q` 语义全都没动** → 回滚只需要 `git checkout` 这几个文件。

---

## 8. 附：为什么"全量"对板子毫无压力（实测数据）

来自 `2026-10-06` 那次实测（板子侧日志）：

```
发送=24~26 帧/秒 (满帧 25)   读失败=0   读耗时均=34~37ms   ← 一条音频链路毫无压力
```

**加上全量文本的额外开销：**

| 项 | 增量 | **全量** | 差 |
|---|---|---|---|
| 消息大小 | ~20 B | ~180 B | +160 B |
| `cJSON_Parse`（主要开销）| ~30 µs | ~100 µs | **+70 µs** |
| `strlcpy` 进缓冲 | 5 B | 150 B | +1 µs |

```
最坏 10 条/秒 → 10 × 100µs = 1 ms/秒 = 0.1% CPU
余量: 音频 DMA 缓冲 128ms ÷ 单次回调 100µs ≈ 【400 倍】
```

**结论：性能从来不是瓶颈，"增量"当初省下的东西可以忽略，却换来了一整类状态 bug。**

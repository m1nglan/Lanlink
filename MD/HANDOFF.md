# Lanlink 交接文档（Handoff）

> 本文件是**接手本项目的入口**。先读这里，再按需读文末「文档索引」。
> 最后更新：**阶段 0 / 1 / 2 / 3 全部完成并上机验证**，只剩**阶段 4（清理加固）**。
> ★★ **协议已升到 V2**（见 [`PROTOCOL.md`](PROTOCOL.md)）：ASR 下行从"增量 `partial`"改成
> **全量 `{"type":"asr"}`**，`revise` 废除，`partial` 从此只属于 llm/openclaw。**读代码前先看那份。**
> ★★ **本轮最大的坑是硬件的**：所有"看起来像软件 bug"的现象（按键误报/ISR 风暴/3.3V 被拉低）
> 根因都是**面包板的地没接回板子**（见 §5.0）。

---

## 1. 项目是什么

**Lanlink** —— 基于 ESP32-S3 的语音助手设备。

```
INMP441 麦克风 → I2S → WiFi → WebSocket 长连接 → 服务器(讯飞 RTASR 识别 / LLM 对话)
                                                      ↓
                                          ST7789 320×170 条屏 + LVGL 9.5 UI
```

业务：按住录音键说话 → 音频流式上传 → 服务器返回识别文本 → 转发给 LLM(DeepSeek/OpenClaw) → 回复。
UI 用 **SquareLine Studio 1.6.2** 设计并导出（5 屏：home / main / Balance / Server / chat）。

---

## 2. 环境实况（**以本节为准，其他文档可能过时**）

| 项 | 值 |
|---|---|
| 芯片 | ESP32-S3 **n16r8**（16MB flash + 8MB Octal PSRAM） |
| IDF | **v6.0.2**（路径 `D:/esp/.espressif/v6.0.2/esp-idf`） |
| LVGL | **9.5.0**（registry 组件，锁定，SquareLine 导出目标） |
| Flash | 16MB，自定义分区表 `partitions_custom.csv`（factory 6MB） |
| PSRAM | Octal **80MHz** 已启用（见 §6 风险） |
| 串口 | 用户自行烧录监控（**不要代为 build/flash，见 §7**） |

### 引脚分配（**勿与 PSRAM 冲突**）

> ★★ **以代码为准** —— 下表按代码逐项核实过。原始定义位置见末尾一列。
> 遇到引脚行为诡异（上电拉低 / 误触发 / 无反应），**第一步就是打开这些头文件对一遍实际连线**。

| 用途 | 引脚 | 定义位置 |
|---|---|---|
| 屏幕 SPI2 | SCLK=**9**, MOSI=**46**, RST=**3**, DC=**8**, CS=**18**, BL=**17** | `main/display/lcd_display.hpp:21-26` |
| 编码器 | A=**45**, B=**41** | `main/drivers/encoder.hpp:22-23` |
| 物理键 | **录音 = GPIO_NUM_2**、**服务 = GPIO_NUM_42**（均按下为低、内部上拉）| `main/main.cpp:40-41` |
| I2S 麦克风 | BCLK=**21**, WS=**47**, DIN=**1** | `main/drivers/i2s_mic.hpp:29-31` |

> ⚠️ **这张表全改过好几轮**（屏幕三版、编码器和按键各一版），早期文档里的
> `SCLK=21/MOSI=20/RST=19/DC=47/CS=48/BL=45`、`A=7/B=15`、`IO10/IO8`、`BCLK=11/WS=12/DIN=13`
> **全部作废**。上表的值才是当前硬件。

⚠️ **用到了几个"特殊脚"，合法但必须知道：**

| 脚 | 特殊之处 | 用在哪 |
|---|---|---|
| GPIO45 / GPIO46 | **strapping**（VDD_SPI 电压 / ROM 打印）| 编码器 A、屏 MOSI |
| GPIO3 | **strapping**（JTAG 源选择）| 屏 RST |
| GPIO39–42 | **JTAG 默认功能**（MTCK/MTDO/MTDI/MTMS）| 编码器 B=41、服务键=42 |

→ 当普通 GPIO 用没问题（**前提是不接 JTAG 调试**），**但上电瞬间有默认电平** ——
将来若再遇到"开机头几秒引脚状态异常"，**先怀疑这几个**。

⚠️ **n16r8 的 Octal PSRAM 占用 GPIO33–37**（D4=33…DQS=37）→ 屏幕/外设**绝不能用 33/34/35/36/37**。
> 历史教训：屏幕旧引脚曾用 35/36/37，与 PSRAM 冲突导致雪花 + PSRAM 位翻转（见 §6）。
> （当前引脚组里没有 33–37 ✓）

### 屏幕 bring-up 旋钮（`main/display/lcd_display.hpp`）

已调试稳定的值，**不要随意改**：
```c
LCD_PIXEL_CLOCK_HZ 40MHz;  LCD_Y_GAP 35;  LCD_MIRROR_Y true;
LCD_RGB_ORDER_RGB 1;  LCD_SWAP_BYTES 1;  LCD_INVERT_COLOR 1;
```
（该屏幕是负显条屏，故 invert=1；RGB 顺序修复过"黄色显示成红"。）

---

## 3. 当前状态

| 项 | 状态 |
|---|---|
| 分支 | `CH_rebuild_logic` |
| 最新提交 | `79fb6da LvglUI 优化_显示流式文字_阶段三完成` |
| 工作区 | 有未提交改动（`MD/REBUILD.md`、`main/business/ui_bridge.cpp`）|
| 阶段 | **阶段 0 + 1 + 2 + 3 完成**；下一个是**阶段 4**（清理加固）|

### 各阶段成果

**阶段 1（已硬件验证）** —— 输入事件化：

- ✅ 转编码器 → LVGL indev → 切屏正常（**无独立任务、无跨任务锁**）
- ✅ 按录音键 → GPIO 中断 + esp_timer 消抖 → 边沿回调正常（**无轮询**）
- ✅ 无 Guru Meditation / 死锁 / Task WDT

期间修掉的 3 个坑（都已记入 §5 / §6，务必读）：
1. **编码器全无反应** —— `lv_group_focus_obj()` 会清掉编辑模式（§5.1）
2. **Task WDT 饿死 IDLE1** —— `pdMS_TO_TICKS(5)` 在 `HZ=100` 下 == 0，`vTaskDelay(0)` 不让出（§6）
3. **按键大抖动** —— 消抖 15ms 不够，本硬件验证值是 50ms（§5.3）

**阶段 0（基础设施）** —— 已补：

- ✅ `main/business/` 已建；`CMakeLists.txt` 的 `SRC_DIRS`/`INCLUDE_DIRS` 已加 `"business"`
- ✅ `ws.cpp::init()` 已钉核（注意还须设 `task_core_id_set`，见 §6）
- ✅ `app_fsm.{hpp,cpp}`、`button.{hpp,cpp}` 已改名 `*.txt` 留档（不再参与编译）

**阶段 2（已硬件验证）** —— ws_keeper + UiBridge + voice：

- ✅ 三条队列（`voice_q` / `resp_q` / `stream_q`）+ asr 回调接线 + UI 消费 timer
- ✅ `ws_keeper_task`（连接生命周期 + 状态上报）、`voice_task`（命令驱动）
- ✅ `main.cpp` 重写：WiFi + WS + 三任务 + 按键接线

**阶段 3（已硬件验证，本轮主体）** —— 真采音闭环 + 上屏：

- ✅ **part A 真采音**：`voice_session` 完整跑通（等连接 → 切 text + 等 `svc_ok` → `start`
  → `mic.start` → 采音循环 → `mic.stop` + `end`）。实测 `共发 284 帧 (=11.4 秒音频), 读失败 0 帧`
- ✅ **part B 上屏**：`ui_bridge.cpp` 里 `chat_view_*` 那一套 —— 按下就出空气泡、逐字流式、
  `is_final` 定格、超限淘汰最老的（`CHAT_MAX_BUBBLES = 10`）
- ✅ **协议升到 V2**（本轮最大改动，见 §6-B 与 [`PROTOCOL.md`](PROTOCOL.md)）
- ✅ 顺手修掉：压缩字体不显示（§6-C）、ws 发送超时单位（§6-I）、55 秒单轮上限（§6-G）、
  按键误报/ISR 风暴（§5.0，根因是**接地**）

### 👉 下一步

进**阶段 4（清理加固）**，清单见 §9。核心几条：
`printf` 已降级过了；剩下 `xQueueCreateStatic` + PSRAM、`UiBridge` 拆分、渲染提速（137ms/轮）。

---

## 4. 架构与重构计划

**完整重构方案见仓库内 [`REBUILD.md`](REBUILD.md)**（就是当前正在执行的计划，含双通道队列设计、任务布局、分阶段步骤、消息结构）。

核心思路（一句话）：**业务从"状态机轮询"改为"事件/命令驱动"**，
`RtAsr`/`Llm`/`WS` 协议层几乎不动（干净无状态），重写的是调度层（`AppFsm` 退役）。

阶段划分：~~0 基础设施~~ → ~~1 输入事件化~~ → ~~2 ws_keeper+UiBridge~~ → ~~3 语音采音闭环 + 上屏~~（**以上全部已硬件验证**）→ **4 清理加固（只剩这个）**。


### 任务/上下文地图（**这是读代码的入口**，阶段 3 后的现状）

| 任务 | 核 | prio | 栈 | 职责 |
|---|---|---|---|---|
| `lvgl` | 1 | 2 | 6K | 渲染 + 编码器 indev + **UI 队列消费 timer(50ms)** ← 气泡上屏也在这里 |
| `ws_keeper` | 0 | 5 | 8K | WS 连接生命周期：建连 / 重连 / ping 保活。**不切服务**（归 voice_task） |
| `voice` | 0 | 6 | 6K | 命令驱动语音会话（**真采音**：等连接 → 切 text + 等 svc_ok → start → 采音 → end） |
| `websocket_task` | 0 | 5 | 4K | **组件自带**：收包 → 同步跑 WS 回调（在 `ws.cpp` 钉核） |
| ~~按键 / 编码器~~ | — | — | — | **无任务**：GPIO 中断 + esp_timer / LVGL indev |

数据流（**跨任务只走队列；发送是同步函数调用**）：

```
按键(中断→esp_timer) --voice_q--> voice_task --asr.start/send_audio/end--> 服务器
服务器 --> websocket_task(回调) --resp_q(状态)/stream_q(完整文本)--> lvgl 任务(UI timer) --> 气泡
```

> ⚠️ 阶段 2 往 **CPU0** 加了 3 个东西（`ws_keeper` prio5、`voice` prio6、组件 `websocket_task` prio5）。
> §6 那个 `pdMS_TO_TICKS` 陷阱已经从 CPU1 搬到 CPU0 —— 新任务一律用"夹紧 tick 值"的写法，
> 否则 IDLE0 会被饿死、Task WDT 照炸。（`drain_queues` 里也已把 1KB 消息体改成 `static` 给
> 6K 栈让位。）

---

## 5. 输入链路（**已全部解决**；★ 5.0-A 是真根因，务必读）

### 5.0 阶段 3 冒出来的两个按键问题 —— 已修（**但只是防线，真根因见 5.0-A**）

**症状**：① 开机 2.4 秒、用户没碰按键，冒出 `[按键] 录音键 松开 → voice_stop`；
② `Guru Meditation Error: Core 0 panic'ed (**Interrupt wdt timeout on CPU0**)`，
backtrace 停在 `button_edge_isr` → `esp_timer_restart` → `esp_timer_impl_get_time`。

**① 误报"释放" —— 根因：`gpio_config()` 之后【立刻】采初值**

```c
ret = gpio_config(&io);                                   // 这一刻才使能上拉
b->last_settled = (gpio_get_level(pin) == active_level);  // 隔几微秒就读
```
引脚那一刻还没稳 → 读到低 → `last_settled` 记成"按下" → 等它稳下来触发一次边沿 →
消抖后对比发现变了 → **误报一条"释放"**。
**修法**：连采到"连续 10 次不变"（最多 100ms）再定初值；并打印实际稳定电平。

**② Interrupt WDT —— 根因：`esp_timer_restart` 从 ISR 里调【安全但不轻】**

读 `esp_timer.c:135-177`，它做的是：
`timer_list_lock()`（`portENTER_CRITICAL_SAFE` = **屏蔽中断**）→ 读 systimer + 64 位乘除
→ **`timer_remove()` 从有序链表摘出来** → **`timer_insert()` 再遍历一次插回去** → 解锁。

按键抖动/引脚噪声时 ISR 被反复重入，每次都要屏蔽一遍中断 → **CPU0 几乎 100% 泡在 ISR 里**
→ 中断看门狗（300ms）永远轮不上 → panic。
**修法**：ISR 里加"**同一个 tick 内只做一次**"的限流（`xTaskGetTickCountFromISR` 只是读变量，
比 `esp_timer_restart` 便宜几个数量级），上限 100 次/秒，对 50ms 消抖完全够。

**诊断手段（已加）**：`button_edge_isr_count()` 累计中断次数 ——
每次上报时打"期间中断 N 次"，并在 voice 的每秒 `level=` 日志里打累计值。
**正常一次按键 = 几次~几十次；几百/几千 = 引脚在噪声里翻转**（接触不良/悬空/上拉没接上）。

### ★★ 5.0-A 这两个问题的【真正根因】是接地（用户的硬件发现）

上面 ①② 我都当作软件 bug 修了（`gpio_config` 后等稳定 / ISR 限流）—— **那些修法有用，但只是防线；
真正的根因是硬件，而且是用户自己查出来的**：

**症状**：ESP32-S3-DevKitC-1 上电后 **3.3V 引脚被拉低约 4 秒**；按键引脚在头几秒被拉低
→ 那条误报的"松开"；引脚在噪声里翻转 → ISR 风暴 → Interrupt WDT panic。

**排查过程（用户做的）**：
1. 只把板子的 **3.3V + GND** 接在面包板上 → **正常**
2. 把整块板插上去（其他 GPIO 也连）→ **不正常**
3. 一路拔到只剩一个麦克风 → **还是不正常**
4. 把板子从面包板上拿下来单独测 → **3.3V 就是 3.3V，正常**
5. ⇒ **结论：面包板上有些元件在把 3.3V 拉低**

**根因**：**面包板上板子的【地】没有真正回到板子的 GND** ——
整个地平面是靠**其他 GPIO 的下拉 / ESD 保护二极管**在维持。

```
正常:  模块 GND → 面包板 GND 轨 → 板子 GND 引脚 → 芯片地
                          ↑ 这条路断了
实际:  模块 GND → [回到 ESP32 的唯一通路只剩]
                  GPIO 引脚 ←→ 内部 ESD 二极管 ←→ 芯片地   ← 电流在打保护二极管
```

**用户重新接线后，全部症状消失。**

> ★★ **教训（这一整轮最值钱的一条）**：
> **"按键误报松开 / ISR 风暴 / 3.3V 被拉低"这几个看起来 100% 像软件 bug 的现象，根因都是接地。**
> 判断信号：**只在接了 GPIO 之后才出问题、只接电源线就正常** → 立刻去查地，别再改代码。
>
> ★ **还要回头质疑历史结论**：§9 记过"PSRAM 有历史位翻转"（当时归因于 80MHz + 屏幕引脚冲突）。
> **如果那也是接地引起的，那条结论方向就是错的 —— 已标"待重新评估"**（见 §9）。

> ⚠️ 剩下**未做**的硬件侧动作：面包板上按 10k 上拉 + **100nF 到 GND**（压噪声）。
> 目前重接线后已经稳了，所以只是备用手段。

### ✅ 5.1 编码器无反应 —— 已修复（源码级定位）

**根因：编辑模式被 `lv_group_focus_obj()` 清掉，旋转退化成 navigate 模式。**

调用链（全部读过 LVGL 9.5 源码确认）：

1. `lv_group_focus_obj()` 函数体**第一件事**就是 `lv_group_set_editing(g, false)`
   （`managed_components/lvgl__lvgl/src/core/lv_group.c:242`）—— 只要它真被调用，编辑模式就没了。
2. 编辑模式 = 旋转时 `indev_encoder_proc` 走 `lv_group_send_data(LV_KEY_LEFT/RIGHT)` → 屏幕对象收到
   `LV_EVENT_KEY` → SquareLine 的切屏事件生效。
   导航模式 = 只 `lv_group_focus_next/prev`，**永远不发 `LV_EVENT_KEY`** → 屏幕毫无反应。
   （`lv_indev.c:1140-1175`）
3. 修复前 `lvgl_focus_active_screen_locked()` 是：当前屏不在组里就 `add_obj`，焦点不是当前屏就
   `focus_obj`。于是：
   - `t≈0` 注册时当前屏是 `ui_main` → 组=`{ui_main}`，焦点=`ui_main`（`lv_group_add_obj` 内部
     `refocus`→`focus_prev` 走的是 `focus_next_core`，**直接赋值 obj_focus，不经过 focus_obj**，
     所以此时 `editing` 还是 1）。
   - `t≈7.5s` 切到 `ui_home` 后：`ui_home` 不在组里 → `add_obj`（组变成 `{ui_main, ui_home}`，
     `add_obj` 只在"组里唯一对象"时才 refocus，两个对象时不 refocus，**焦点仍停在 ui_main**）
     → 焦点 ≠ 当前屏 → **`lv_group_focus_obj(ui_home)` → `editing` 被清成 0**。
   - 之后每轮 read_cb 焦点都已是 `ui_home`，条件不再成立，**`editing` 永久停在 0**。
   → **开机 7.5 秒后转编码器：ISR 正常、indev 正常、格数正常，但屏幕永远不动。**

**为什么旧方案没这问题**：重构前那个 `lvgl_port_send_encoder_dir()`（**已删除**）直接调
`lv_group_send_data()`，**绕过了编辑模式判断**，所以同一套 group 设置它能用；
改成 indev 后依赖编辑模式，就踩上了 —— 这也是当初最难看出根因的原因。

**修复**（`main/display/lvgl_port.cpp::lvgl_focus_active_screen_locked`）：
- 屏幕一变就 `lv_group_remove_all_objs()` 再 `add_obj(当前屏)` → 组里**只留当前屏**，
  消灭"旧屏残留导致每轮都走 focus_obj"的坑；
- 最后**无条件重申** `lv_group_set_editing(g, true)`（函数内部幂等，`editing` 相同直接 return）→ 自愈。
- 稳定态下两个条件都成立 → 整块跳过，**零额外开销、无额外重绘**。

### ✅ 5.2 物理按键无反应 —— 已解决（根因是引脚认错，不是软件）

现象：按 IO10/IO8 无任何反应，串口连"裸电平 1→0"都没有；用户手动拉低也没反应。

排查结论（**软件侧全部排除**）：
- 编码器与按键用的是**同一条 GPIO ISR 服务**（`gpio_install_isr_service` +
  `gpio_isr_handler_add`）。编码器 ISR 确认工作（`enc_raw` 只由 ISR 累加，ISR 不跑屏幕根本不会转）
  → **中断分发层是好的**。
- 我一度怀疑 IDF 的核错配：`gpio_intr_service` 按 `isr_core_id` 读中断状态寄存器
  （`gpio.c:520`），而 `gpio_install_isr_service()` **本身不设置** `isr_core_id`
  （初值 `GPIO_ISR_CORE_ID_UNINIT=3`）。若它一直是 3，ISR 会去读 CPU1 的状态位而中断却在
  CPU0 使能 → 一个都不会触发。**但** `gpio_install_isr_service()` 内部会调
  `gpio_isr_register()`，后者在 `gpio.c:631-632` 把 `isr_core_id = xPortGetCoreID()`
  并在**同一个核**上注册 → **一致，无错配**。
- 旧轮询驱动 `button.cpp` 与 `button_edge_init` 的 `gpio_config` 参数**完全一样**
  （只差 `intr_type`：`DISABLE` vs `ANYEDGE`），上拉都是开的。同一焊盘以前读得到，
  没有理由现在读不到。

⇒ 结论：**"拉低没反应"只可能是拉的那个脚不是 GPIO10/GPIO8**（排针丝印/序号认错、焊错点、焊盘坏）。
用户修正后按键即正常工作。

**教训（下次直接照做）**：诊断必须能区分"**电平没进芯片**"和"**电平进了但中断没触发**"。
做法是同时给出 ① 裸电平**变化事件**日志（高频采样，一变就打印，别只在汇总里印当前值）
+ ② "曾经读到过低电平"的**锁存位**（不受采样时机影响）。
只有"每 500ms 打一次当前电平"会把这两种完全不同的故障混为一谈。

### ✅ 5.3 按键抖动 —— 已解决（消抖时长回归）

阶段 1 初版 `BTN_EDGE_DEBOUNCE_MS = 15`，实测一次按下会打出多条 `按下/释放`。

**回归原因**：旧轮询驱动 `button.hpp` 用的是 `BTN_DEBOUNCE_MS(10) x BTN_DEBOUNCE_N(5)`
= **50ms 连续稳定**，那才是本硬件上验证过（含提交 `6fa1fc5「修复io8抖动」`）的值。
重启式消抖的固有弱点：机械抖动的**安静期只要 > 消抖时长**，就会误报一次。

→ 已把 `BTN_EDGE_DEBOUNCE_MS` 改回 **50**（重启式消抖，等价于旧版"连续稳定 50ms"）。

权衡：按下延迟 ≈ 本值；松开→voice_stop 最坏 ≈ 本值 + 帧边界 40ms（阶段 3）。
若 50ms 仍抖，先怀疑**电气**（内部上拉只有 ~45kΩ，长线 + 40MHz SPI 串扰）：
外挂 10k 上拉 + 100nF 到 GND，而不是继续加大消抖。

### 已排除的假设（省得重查）

- ❌ "indev 没绑 display" —— **错**。`lv_indev_create()` 内部已 `indev->disp = lv_display_get_default()`（`lv_indev.c:134`）。
- ❌ "`button_edge.cpp` 没被编译" —— 那是更早一轮的 CMake glob 问题，已 touch CMakeLists 解决。
- ❌ "`lv_group_focus_obj` 清编辑模式的路径不成立" —— 注册那一刻确实逃过去了（`lv_group_add_obj` 内部
  refocus 走 `focus_next_core` 直接改 `obj_focus`），**是切屏之后才踩上的**。别只查注册路径。

### 两个"设计上本就无反应"的点（**别当成 bug**）

1. **启动屏 `ui_main` 不处理按键**：只处理 `SCREEN_LOADED`，然后**延迟 7000ms** 才切到 `ui_home`
   （`ui_home` 才处理 `LV_KEY_LEFT/RIGHT`）。→ **开机前 ~7.5 秒转编码器本就不该有反应。**
2. **屏幕动画期间输入被屏蔽**：`lv_indev_read()` 开头 `if(indev->disp->prev_scr != NULL) return;`
   （`lv_indev.c:244`）—— 注意这一句在 `read_cb` **之前**，所以动画期间任何 read_cb 侧计数
   也会一起冻住，别误判成"indev 没被轮询"。

---

## 6. 源码级验证过的技术事实（**都是踩过的坑，务必遵守**）

这些是读 LVGL / IDF / esp_websocket 源码确认的，不是猜测：

> **本节字母索引**（本轮新增的 6-x 小节）：
> **6-A** 消息载荷 / 队列内存（下面那张表）｜ **6-B** 协议 V2 ｜
> **6-C** 🐛 压缩字体坑 ｜ 6-D **LVGL 布局 x/y/align 规则（在下面 "LVGL 9.5 具体行为" 表里）** ｜
> **6-E** 聊天屏生命周期 / 悬空指针 ｜ **6-F** 气泡三层结构 ｜
> **6-G** 55 秒单轮上限 ｜ **6-H** 音频链路实测基准 ｜ **6-I** 本轮修的其它 bug

### 架构级 4 坑（重构时必须遵守，详见 REBUILD.md）

1. **`esp_websocket_client` 自己建任务收包**（`xTaskCreatePinnedToCore`，默认 `tskNO_AFFINITY` prio5）
   → 所有 WS 回调（partial/final/reply）**跑在那个组件任务里，不是你的 ws_task**。
   → `ws.cpp::init()` 必须补 `.task_core_id = 0; .task_prio = 5;` 否则会跑 CPU1 抢 LVGL。
2. **组件 TX/RX 共用锁**（send 走 `tx_lock+lock`）→ 发送方需与收包任务同核(CPU0)降低锁竞争。
3. **LVGL 锁是非递归互斥量**（`xSemaphoreCreateMutex`）→ **LVGL 上下文内（read_cb / lv_timer，已持锁）
   绝不能再调自带 `lvgl_port_lock()` 的函数**，否则自死锁。用无锁内部函数。
4. **asr/llm 必须全局常驻**：回调 ctx 生命周期=系统；不能放任务栈/循环里重建。

### LVGL 9.5 具体行为

| 事实 | 出处/含义 |
|---|---|
| `lv_label_set_text` **总是** free+malloc+重绘，**不比较内容** | → 必须靠队列"非空才刷"避免无谓开销 |
| `lv_label_set_text` **内部会复制**文本到 label | → **不需要自己另开 char 数组存文字**；删 label 时 `lv_free` 自动回收 |
| 编码器 indev 需 `lv_indev_set_group()` 绑组 | 否则 `indev_encoder_proc` 拿不到焦点对象 |
| **编辑模式 vs 导航模式**：`lv_group_set_editing(g,true)` 才发 `LV_KEY_LEFT/RIGHT` 给聚焦对象；否则转动只移动焦点 | SquareLine 的切屏事件监听的是 `LV_EVENT_KEY`，故必须编辑模式 |
| ★ **`lv_group_focus_obj()` 内部会 `lv_group_set_editing(g,false)`**（`lv_group.c:242`，函数第一件事）—— **只要它真被调用，编辑模式就被清掉** | 用编码器 indev 时千万别随便调 `focus_obj`；`lv_group_add_obj()` 走的是 refocus→`focus_next_core`（直接改 `obj_focus`，**不清**编辑模式），两者行为不同，别以为等价 |
| 转动 → 焦点对象收到 `LV_EVENT_KEY`；按 ENTER 松开 → `LV_EVENT_CLICKED` | 做"按钮触发"就挂 `LV_EVENT_CLICKED` |
| **`lv_screen_active()` 是聚焦对象**才能收到键 | SquareLine 把切屏事件挂在**屏幕对象**上 |
| ★★ **一个对象的 `x`/`y`/`align` 是否生效，取决于【它的父对象】有没有 layout** | 源码：`lv_obj_pos.c:777` `lv_obj_refr_pos()` 开头就是 `if(lv_obj_is_layout_positioned(obj)) return;`，而 `:356` 的实现是"父对象有 layout → true"。**父对象有 flex/grid 时，孩子的 x/y/align 全被忽略** |
| flex 布局**直接改 `item->coords`** | `lv_flex.c:555-567`：`diff_x = abs_x - item->coords.x1 + ...; item->coords.x1 += diff_x;` —— 之前设的 x/y/align 一律被覆盖。所以 SquareLine 导出里那些"无效坐标"删不删都不影响，但留着会让预览和真机不一致 |
| **flex 不支持"每条单独对齐"** | `cross_place` 是**整容器一个值**，所有孩子一样 → 做不到"我的靠右、对方的靠左"。破解办法见 §6-F（每条套一层 wrapper） |
| `lv_obj_create(NULL)` 会创建一个**新屏幕**，不是子对象 | ★ 拿可能为 NULL 的指针当父对象之前**必须先判空**，否则会静默造出一个屏幕 |
| `lv_obj_is_valid(obj)`（`lv_obj.c:450`）**只做指针比较、不解引用入参** | 遍历所有 display 的屏幕对象树比对指针 → **对悬空指针也是安全的**。这是它最宝贵的性质，见 §6-E |
| `lv_obj_update_layout()` 在 `lv_timer` 回调里调用是安全的 | `lv_obj_pos.c:383` 开头 `if(update_layout_mutex) { LV_LOG_TRACE("Already running, returning"); return; }` → **重入直接早退，不会死锁** |
| `LV_ALIGN_TOP_RIGHT` 会被算进高度 | `lv_obj_pos.c:1492` 把 `LV_ALIGN_DEFAULT / TOP_RIGHT / TOP_MID / TOP_LEFT` 一起归入 "Normal top aligns"，`child_res = child->coords.y2 - obj->coords.y1 + 1` → 父对象的 `SIZE_CONTENT` 高能正确等于这个孩子的高。**换成 `LV_ALIGN_CENTER` 会走 `default` 分支，结果不同** |

### IDF / FreeRTOS

| 事实 | 含义 |
|---|---|
| `esp_timer_restart()` 对**未启动**的 timer 返回 `INVALID_STATE` 且**不启动它** | → ISR 里首次边沿须用 `start_once`，之后才 `restart` |
| `esp_timer_restart/start_once` 是 `ESP_TIMER_IRAM_ATTR` | ISR 内可安全调用 |
| **`gpio_install_isr_service()` 全芯片只能成功装一次**，第二次返回 `ESP_ERR_INVALID_STATE` 并**打一条 `E` 级日志**（`gpio.c:537`） | 多个驱动都要 GPIO 中断时，统一走 `drivers/gpio_isr_once.hpp` 的 `gpio_isr_service_ensure()`（幂等，函数内 static 保证全程序单实例）→ 启动日志不再出现误导性的 `E gpio: GPIO isr service already installed` |
| **`gpio_intr_service` 按 `isr_core_id` 读中断状态寄存器**（`gpio.c:520`），而该值由 `gpio_isr_register()` 设为调用者所在核（`gpio.c:631-632`） | 若哪天出现"中断使能在 A 核、状态却在 B 核读"的错配，GPIO 中断会**全部静默失效**。排查时记住 `gpio_install_isr_service()` **自身不设置** `isr_core_id`（初值 `GPIO_ISR_CORE_ID_UNINIT=3`） |
| `_Atomic` 是 C11，**C++ 不认**（IDF 用 `gnu++26`） | 用 `std::atomic` 或 `portMUX_TYPE` 临界区 |
| **C++20 起 `volatile` 的 `++` / `--` / 复合赋值（`+=` 等）被弃用**，IDF 用 `gnu++26`，默认 `-Werror=volatile` 会**直接编译失败** | 已在 `main/CMakeLists.txt` 加 `-Wno-error=volatile` **降级为警告**（与已有三条同一个 `target_compile_options` 块）→ 代码可继续写 `s_x++`。若哪天真要清零警告，改成 `x = x + 1;` 即可（`x += 1` 同样被弃用） |
| `std::atomic` 的 `fetch_add` 在 Xtensa 不保证无锁 | **ISR 内用 `portENTER_CRITICAL_ISR` 更稳** |
| **`CONFIG_FREERTOS_HZ=100`（本项目实测值）→ 1 tick = 10ms，`pdMS_TO_TICKS(1..9) == 0`** | **`vTaskDelay(0)` 只 yield，不让 CPU 给更低优先级任务**。任何"夹紧到 ≥5ms 再 `pdMS_TO_TICKS`"的写法在 HZ=100 下等于**没有让出**。必须夹紧 **tick 值**：`TickType_t t = pdMS_TO_TICKS(ms); if (t < 1) t = 1;`。踩坑现场：`lvgl_port_task` 因此 100% 占住 CPU1 → IDLE1 饿死 → Task WDT(5s) 触发 |
| `spi_bus_dma_memory_alloc(host,sz,caps)` 支持 `MALLOC_CAP_SPIRAM` | SPI2 + GDMA 可直读 PSRAM 绘图缓冲（自动 cache sync） |
| **`SRC_DIRS` + `file(GLOB)` 只在 CMake 配置阶段扫描** | **新增源文件后必须 `touch main/CMakeLists.txt`**，否则新文件不参与编译→链接期 undefined reference |

### esp_websocket_client（**收发链路的全部坑，都是读组件源码确认的**）

| 事实 | 含义 |
|---|---|
| **钉核必须设 `task_core_id_set = true`**（`esp_websocket_client.h:129`）。组件实现是 `if (config->task_core_id_set) cfg->task_core_id = ...; else cfg->task_core_id = tskNO_AFFINITY;`（`esp_websocket_client.c:369-372`） | ⚠️ **REBUILD.md 漏了这个字段**：只写 `cfg.task_core_id = 0` **无效**，会被当成"未设置"而走 `tskNO_AFFINITY`。因为 0 本身是合法核号，所以组件专门加了个 bool 开关。`ws.cpp::init()` 已按要求写全 |
| 组件任务默认 `WEBSOCKET_TASK_CORE_ID=tskNO_AFFINITY`、`PRIORITY=5`、`STACK=4K`（`esp_websocket_client.c:33-35`） | `task_prio/task_stack` 留 0 会用这些默认值 |
| **回调跑在组件任务里，且是同步内联**：`dispatch_event()` 先 `esp_event_post_to(...)` 再**紧接着** `esp_event_loop_run(handle, 0)`；而 event loop 建的时候 `.task_name = NULL`（不建任务）→ 在调用者(websocket_task)上下文同步执行 | ✅ REBUILD 坑#1 属实。我们的 `ws_keeper`/`voice_task` **完全不参与收包** |
| **回调期间持有 `client->lock`**：主循环是 `xSemaphoreTakeRecursive(client->lock)` → `recv()` → 里面调 `dispatch_event()`（`esp_websocket_client.c:1397-1402` / `1118`） | ★★ **回调里绝对不能阻塞**。所以 `stream_q` 用 `xQueueOverwrite`（天生不阻塞），`resp_q` 必须 `xQueueSend(..., 0)`。**用 `portMAX_DELAY` 会让整个 WS 收发死锁**（TX/RX 共用这把锁）|
| ★ **组件在 `poll` 时是【放锁】的**（`esp_websocket_client.c:1380-1402`：先 `xSemaphoreGiveRecursive` → `esp_transport_poll_read(..., 1000)` → **有数据才** `TakeRecursive` → `recv` → `Give`）| 所以 `voice_task` 的 `send_audio` **不会**被"组件在等数据"卡住 —— 它只可能被"**单次回调的耗时**"卡住。排除了一大嫌疑 |
| 🐛 **已修** WS 回调里的 `printf`/`fflush(stdout)`（`rtasr.cpp::accumulate`/`revise`） | 两个后果实测都踩到了：① 持 `client->lock` 打串口 → 拖住 `voice_task` 的 `send_audio`；② 往 UART 灌大量字符 → **UART 中断频繁触发**（那次 Interrupt WDT panic 的 `EPC1` 正是 `uart_hal_write_txfifo`），在 CPU0 上和 GPIO 中断一起把中断看门狗饿死。已降级为 `ESP_LOGD`（文字本来就通过 `m_cb` 交给 UI 了，再打一遍纯浪费）|
| `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` **未开** → `client->lock` 一把递归锁 TX/RX 共用（`esp_websocket_client.c:816`） | ✅ REBUILD 坑#2 属实 → `voice_task`/`ws_keeper`/`websocket_task` **必须同核 CPU0**。递归锁保证了**同任务重入不死锁** |
| `WS::set_handler()` 按 `type` **自带去重**；`WS::deinit()` **不清 handler 表** | → **重连不需要重新 `asr.attach()`**，handler 表挂在 WS 单例上会一直保留，`init()` 会把它们挂到新 client 的 event loop 上 |
| 组件配了 `disable_auto_reconnect = true` / `disable_pingpong_discon = true` | **重连和保活必须由我们的 `ws_keeper` 负责**，别指望组件 |
| 🐛 **已修** `esp_websocket_client_send_*` 的 `timeout` 是 **RTOS ticks，不是毫秒**（`esp_websocket_client.h:279` 明写 "in RTOS ticks"；组件 `:742` 才做 `timeout * portTICK_PERIOD_MS`） | **原 bug**：`ws.cpp` 把 `timeout_ms` 直接传进去 → `HZ=100` 时 `1000` 变成 **10000ms = 10 秒**。代价很具体：音频发一帧最多阻塞 10 秒，而 I2S DMA 缓冲只有 **128ms** → 这一整段音频全丢。**修法**：`ws.cpp` 加 `ws_timeout_ticks()` 统一换算，并把下限夹到 **1 tick**（`pdMS_TO_TICKS(1..9)==0`，而组件里 `timeout=0` 表示"不等待"而非"很短"）。现在 `WS_SEND_TIMEOUT_MS(1000)` 真的是 1 秒了 |
| ⚠️ **发送阻塞 vs DMA 余量（尚未处理，待定）** | 修完单位后音频发送超时 = 1 秒，而 DMA 只有 **128ms** → 真卡住一次仍会丢约 0.9 秒音频；而且 `send_audio` 失败会**直接中止会话**（`voice.cpp:224`）。要再收紧就两条：① 把**音频**的发送超时单独降到 ~100ms（< DMA 容量）；② 把"失败"改成**丢这一帧继续**（连续丢太多才 abort），而不是立刻 abort。**先不动**：一帧 1.28KB，局域网正常只要几毫秒，实测不触发 |

### 业务层铁律

| 事实 | 含义 |
|---|---|
| ★ **`partial` 的路由只认 `WS::m_service`**（`ws.cpp:313`），而 `RtAsr::switch_service()` **只发消息、不改 `m_service`**（`rtasr.cpp:42`） | 切服务必须**成对**：`ws.set_service("text")` **+** `asr.switch_service("text", ...)`。漏第一句 → 语音 partial 被送去 chat 槽位（阶段2 是 NULL）→ `if (h != NULL)` 无 else、不打日志 → **识别文字静默消失**。旧代码在 `app_fsm.cpp:125-126` 就是成对写的 |
| `RtAsr::handle_partial/final/revise` 三者只差"取到 text 之后干什么" | 已抽成 `extract_text()` + 三个薄壳（`rtasr.cpp`）。**别把 `revise` 并进 `final`**：`revise` 故意不触发 `m_cb`，合并会让 UI 收到假的"说完了" |
| `WS::dispatch_msg` 里 `partial` **不走查表**，因为 `type=="partial"` 被"语音"和"LLM"两个服务共用，一维 `type` 分不出来 | 见上一条 |
| ★ **现在只有 `voice_task` 切服务**（会话开头），`ws_keeper` **故意不切** | `ws_keeper` 原来在重连后会补一句 `set_service`+`switch_service`，那是**旧轮询式调度器的补丁**（怕"语音已经过去了但服务还没切"）。现在切服务和发 `start` 在同一个任务里顺序执行，中间还隔着 `svc_ok` 握手 → 那个场景不可能发生。**删掉它还有个好处**：`ws_keeper` 不再产生 `svc_ok` → `VOICE_SVC_ACKED` 的唯一生产者就是 `voice_session` 自己，不会被重连的确认误唤醒 |
| 🐛 **已修** `voice_session` 的"等连接"循环（最长 15 秒）**不消费 `voice_q`** | 期间 `voice_task` 是 `voice_q` 唯一的消费者，一不消费：① 深度只有 4，按几次就满 → 后续按键被 `xQueueSend(...,0)` 丢弃；② **`VOICE_STOP` 也一起丢 → 用户松手被忽略** → 连上后照常录一段用户不要的会话。实测踩过开机 2.4s（还没连上）打出 `voice_q 满, VOICE_STOP 丢弃`。已抽 `voice_q_drain()` 并在**所有等待循环**里调用（发现 STOP 就取消本轮） |
| ★ **`voice_session` 会等服务器回 `svc_ok` 才发 `start`**（`wait_svc_ok()`，上限 300ms） | 信号链：服务器回 `svc_ok` → `ws.cpp:331` 调 `m_svc_ok_cb` → `UiBridge::on_svc_ok` → 投 `VOICE_SVC_ACKED`。**实测网关对重复的 `svc` 也照回 ack（61~63ms，5/5）** → 300ms 有 5 倍余量，不需要"超时也放行"的兜底 |

### 6-A 消息载荷 / 队列内存（`bus_msg.hpp`）★ 改动前务必先读

三条通道（`voice_q` / `resp_q` / `stream_q`）。**V2 起 `stream_q` 只装"完整文本"。**

| 事实 | 含义 |
|---|---|
| ★★ **V2: `stream_q` 上的 `text` 永远是【完整文本】，不是增量** | 消费者**零累积状态** —— 收到什么就 `lv_label_set_text` 什么。V1 那套"partial 追加 / revise 覆盖 / final 覆盖"三条语义不同的路已废除（实测出过"`revise` 的片段把累积好的整句冲掉，屏幕上只剩 `结果` 两个字"） |
| **`stream_msg_t` 加了 `is_final`** | 把"流式"和"完成"合并到同一条消息里 → `resp_q` 不再需要 `RESP_ASR_FINAL`/`RESP_LLM_FINAL`（**已删**） |
| **`BUS_TEXT_LEN` = 2048**（**只用在 `stream_q`**，深度 1 覆盖式 → 只占一份） | 所以开大很便宜：512→2048 只多花 1.5KB。**"长文本只走覆盖式队列"** 是这条链的关键 —— 长文本若走深 16 的 `resp_q`，2048 就要 32KB，根本做不到 |
| 🐛 **`BUS_TEXT_LEN_SHORT` = 64**（`resp_q` 用，深 16）—— **V1 遗留的洞，已缩** | V1 时 `RESP_ASR_FINAL` 走 `resp_q` 装完整句 → 得给 512。V2 后识别结果全走 `stream_q`，那个 512 就变成空气：**`512 × 16 = 8384 B`**，而每条实际只用 `u.sta` 的 4 字节。缩到 64（≈21 汉字，够放"服务切换超时"这类提示）后 `resp_q` 从 **8384 → 1216 B** |
| **队列总计 3304 B**（V1 是 8932 B） | `stream_msg_t` 2056×1 + `resp_msg_t` 76×16 + `voice_cmd_msg_t` 8×4。代码里留了钉子注释："**别照 V1 的老尺寸改回去**；真需要长文本走 `stream_q`，`resp_q` 深 16，**这里每 +1 字节就是 ×16**" |
| `resp_msg_t` 的载荷是 **union**（`u.text[64]` / `u.i32` / `u.f32` / `u.sta`），**`kind` 决定读哪一项** | 消费端**必须**先看 `kind` 再取 `u` —— 拿状态类去读 `u.text` 会打出乱码（`drain_queues` 里已按 kind 分支） |
| **union 成员必须是"平凡类型"，且不得含指针** | 队列靠 `memcpy` 搬字节，只保护这块内存本身。放 `char*` 等于把值拷贝退回成"指针 + 一块无人保护的内存"（悬空/被改写）。要放字符串就用**内联定长数组** |
| **状态统一走 `status_kind_t`**（CONNECTED/DISCONNECTED/RECONNECTING/STARTED/ENDED/ABORTED） | 投递用 `post_resp_status(kind, status)`。**标签仍按服务分**（`RESP_ASR_STATUS` / `RESP_WS_STATUS`）以保留"谁报的"。★ **聊天屏就是靠 `RESP_ASR_STATUS` 的 `STARTED`/`ENDED` 驱动气泡生命周期的**（按下就出空气泡、结束收尾，见 §6-F） |
| 三个投递接口最后都走 **`post_resp_msg()`** 入队 | 那是 `resp_q` 的**唯一入队点**：`timeout=0` 的硬要求和"满了告警"只写一份。加新载荷类型时，调用方自己填 `resp_msg_t` 再调它即可，**不用改 UiBridge**。（`post_resp` 现在**没有调用者** —— 留给未来 `RESP_STATUS` 短提示用） |
| `voice_cmd_msg_t.arg` 是**标量参数槽位**（4 字节），当前**全部传 0、没人读** | 装不下文本；**更不要拿它塞指针**（32 位机上 `int32_t` 和指针同宽，编译通过但会引入悬空） |
| 状态上报要**去抖**（见 `ws_keeper.cpp::report_ws_status`） | 只在**状态变化**时投一条，理由有两条：① 状态没变还重复投，UI 每轮会刷出同一句话（白白重绘）；② `DISCONNECTED` 只在"连上过又断了"时报，开机没连上不报 —— 免得 UI 一上来就说"断线"。**注意：这不是"防队列溢出"** —— 消费者 `lv_timer` 每轮把 `resp_q` 取空，重连循环 5 条/秒远低于消费能力 |

### 6-B ★★ 协议 V2：ASR 从"增量"改成"全量"（**完整规范见 [`PROTOCOL.md`](PROTOCOL.md)**）

| 事实 | 含义 |
|---|---|
| ASR 下行改用 **`{"type":"asr","text":"<完整当前句>"}`**，**不再是** `partial` 增量 | 板子侧 `RtAsr::store_and_notify()` 一律 **`strlcpy`（覆盖）**，**零累积状态** |
| `{"type":"final"}` 也必须是**完整句**（不是最后那一小段） | 网关**违反过**这条：实测收到 `{"type":"final","text":"结果"}`，把累积好的整句冲成了两个字 |
| **`revise` 已废除** | `asr` 每次都是全量（自带修正），不需要单独的修正消息。网关**也违反过**：实测它在发 `revise`，而且内容有时是**片段**（`revise "喂"`），板子按"整体替换"用它覆盖 → 累积全丢 |
| **`partial` 从此只属于 `llm`/`openclaw`**（仍是增量） | 但**累积发生在板子的 `Llm` 驱动里**（`llm.cpp::handle_partial` 里 `strlcat`），**发布到总线时已经是完整回复** |
| ★ **为什么 LLM 的累积必须在驱动侧做** | 总线上的 `stream_q` 是**深度 1 覆盖式**。若把增量原样放进去、让 UI 侧累积，UI 每 ~137ms 才取一次 → **中间的增量被覆盖 → 永久丢字**。在驱动侧累积则每条 partial 都同步并进 `s_stream`，覆盖多少次都无所谓 |
| **副作用（正向）**：`ws.cpp::dispatch_msg` 里那套"`partial` 按 `m_service` 分流"的特判**整体删掉了** | V1 因为 `partial` 被 text 和 llm 共用，一维 `type` 分不出来；V2 里 `asr` 只属于 text、`partial` 只属于 llm → 一张 `type` 表就够。`set_partial_handler` 及其 4 个成员也一并删除 |
| **实测证据（硬件已验证）** | `[流式] kind=0 final=0 text="..."` 逐步变长（全量替换，不是重复叠加）→ 最后 `final=1` 时整句完整 |
| ⚠️ **迁移顺序不能反** | 必须**网关先改**（能发完整 `asr`），**再**烧板子。反过来（网关还发 `partial`、板子已改成覆盖）屏幕只会显示最后一个增量，更糟 |

### 6-C 🐛 压缩字体坑（排查了很久，**必须留档**）

**症状：chat 屏气泡「框的长度对，但一个字都不画」。**

| 事实 | 出处 / 含义 |
|---|---|
| 根因：`ui_font_ch14.c` 的 **`.bitmap_format = 1`**（= `LV_FONT_FMT_TXT_COMPRESSED`），而 sdkconfig 里 `CONFIG_LV_USE_FONT_COMPRESSED` **未开** | 一句话：**"我是压缩的" + "我不会解压"** |
| 链路：`lv_font_fmt_txt.c:109` 走 `bitmap_format != PLAIN` 的 `else` → `#if LV_USE_FONT_COMPRESSED` 为 0 → `LV_LOG_WARN("Compressed fonts is used but LV_USE_FONT_COMPRESSED is not enabled")` → **`return NULL`**（`lv_font_fmt_txt.c:207` 附近） | 一个像素都不画 |
| ★★ **为什么"尺寸还是对的"** | **度量**走 `lv_font_get_glyph_dsc_fmt_txt()`，它**不看 `bitmap_format`**；**像素**走 `lv_font_get_glyph_bitmap_fmt_txt()`，它**看**。→ **两条路互相独立** |
| ★★ **可复用的判据** | **"尺寸对、字不画" → 先怀疑【像素来源】，不要查尺寸计算。** 这个组合是这个 bug 的唯一特征 |
| 为什么只有聊天屏中招 | 其它 7 个项目字体（`balance22`/`date22`/`icon18`/`time40`/`time64`/`updown10`/`weather18`）**全是 `bitmap_format = 0`**（明文），明文路径根本不碰解压器 |
| ⚠️ **文件头那行 `Opts: ... --no-compress --no-prefilter` 是假的** | 那是转换器写的日志，**与实际不符**（实际数据是压缩的，实测 86% 体积）。**以 `.bitmap_format` 为准，别拿那行当依据** |
| **修法（已采用第一个）** | ① `sdkconfig` **和** `sdkconfig.defaults` 里都加 `CONFIG_LV_USE_FONT_COMPRESSED=y`（两个都要 —— `defaults` 只在生成新 `sdkconfig` 时生效）；② 或在 SquareLine 里取消压缩重新导出（`bitmap_format` 变 0 → 不需要解压器、**渲染更快**，代价是编译后多约 90KB flash） |
| 已把整条因果链写成注释放在 **`main/lvgl/fonts/ui_font_ch14.c` 顶部** | 以后重新导出该字体时，看到注释就知道要查 `.bitmap_format` 这一行 |
| ⚠️ 排查时踩的自己的坑 | 我曾用脚本统计"实际字节 ÷ 明文应有字节"来判断是否压缩 —— **公式错了**（LVGL 字体位图按行字节对齐，且连已知正常的 `date22` 都对不上，只有 94%）。**教训：拿一个已知正常的样本做对照，否则测量本身就是噪声** |

### 6-E 聊天屏的屏幕生命周期 + 悬空指针（**踩过**）

| 事实 | 出处 / 含义 |
|---|---|
| SquareLine 的屏幕是**按需创建 + 离开即销毁** | `ui_chat.c:40` 注册 `LV_EVENT_SCREEN_UNLOADED` → `scr_unloaded_delete_cb` → `lv_obj_del_async` → `ui_chat_screen_destroy()` → **所有 `ui_*` 全局置 NULL** |
| ⚠️ **陷阱：指针非 NULL ≠ 对象还活着** | 我们记在 `s_bubbles[]` / `s_live_label` 里的指针，屏幕一销毁就**全变悬空** → 下一次 `lv_obj_del(s_bubbles[0])` 就是 **use-after-free** |
| **两道保险**（`ui_bridge.cpp::chat_view_sync_screen()`） | ① **换屏检测**：`s_ctx_hooked != ui_contextpanel` → 说明旧屏销毁、新屏刚建 → 清空所有记录 + 顺手删掉 SquareLine 预置的占位气泡；② **`lv_obj_is_valid()` 兜底** |
| ★ **`lv_obj_is_valid()`（`lv_obj.c:450`）只做指针比较、不解引用入参** | 遍历对象树比对指针 → **对悬空指针也是安全的**。这是它在这个场景下最宝贵的性质 |
| ⚠️ **必须判空才能当父对象** | `lv_obj_create(NULL)` 会创建一个**新屏幕**，不是子对象。所以 `chat_view_*` 一律先 `if (ui_contextpanel == NULL) return;`（并打日志） |
| **不在聊天屏时收到识别结果 → 直接丢弃**（只留控制台 `[上屏] 当前不在聊天屏, 丢弃显示`） | 已知限制，**没做"回到聊天屏再补上"**。要做的话需要一个 PSRAM 环形缓冲存最近文字、回来时重放 |

### 6-F 气泡三层结构（**wrapper 破解 flex 的"不能单独对齐"**）

```
ui_contextpanel  (flex column, 可滚动)
└── roll   (wrapper: 310 宽 × SIZE_CONTENT 高, 透明)             ← flex 的孩子 → x/y/align 无效
    └── panel (气泡: SIZE_CONTENT, align=TOP_RIGHT, 绿底)         ← 父无 layout → align 生效 ✓
        └── label (文字: SIZE_CONTENT, max_width 280)             ← 文字在这
```

| 事实 | 含义 |
|---|---|
| 为什么要 wrapper | flex 不支持 per-child 对齐（见 §6 LVGL 表）。**wrapper 是 flex 孩子（被 flex 摆），panel 是 wrapper 孩子而 wrapper 没有 layout → panel 的 `align` 生效** ✓ |
| **尺寸链条（`LV_SIZE_CONTENT` 全靠内容撑开）** | `label` 由文字度量撑开 → 加 padding(8/8/6/6) 得 `panel` → `panel` 的 align 撑开 `roll` 的高度（依据 `lv_obj_pos.c:1492`，见 §6 LVGL 表）→ `ui_contextpanel` 依次往下堆（间距 = 容器 `pad_row`） |
| **生命周期** | **删父删子**：`lv_obj_del(roll)` 连带删 panel + label |
| **文字不用自己开 char 数组** | `lv_label_set_text` 让 **LVGL 自己拷一份**内部管理，删 label 时自动回收 |
| **超限淘汰最老的** | `CHAT_MAX_BUBBLES = 10`；满了就 `lv_obj_del(s_bubbles[0])` + 数组左移 |
| **`lv_label_set_text` 不比较内容**（`lv_label.c:981`） | 同内容也会 free+malloc+重绘 → 靠"`stream_q` 非空才刷"避免白开销 |
| **气泡被"会话状态"驱动，不是被文字驱动** | `RESP_ASR_STATUS` + `STARTED` → **按下就建一个空气泡**（`chat_view_begin_asr`）；`ENDED`/`ABORTED` → 收尾，**若整轮一个字都没出就把空气泡删掉**（`chat_view_end_asr`）。理由：按下到服务器回第一个字可能一两秒，中间没反馈用户会以为坏了 |
| ★ **文字放 PSRAM 的方案当前用开关关着**（`CHAT_TEXT_IN_PSRAM = 0`，`ui_bridge.cpp`） | 路径是 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` + `lv_label_set_text_static()`。**当初以为它坏了，其实根本是上面 §6-C 那个字体坑** —— 等真需要省内部 SRAM 时可以再把开关打开 |
| ⚠️ **为什么 `lv_label_set_text` 的文字进不了 PSRAM** | LVGL 配的是 CLIB malloc（`CONFIG_LV_USE_CLIB_MALLOC=y`）= 标准 `malloc`，而 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384` → **小于 16KB 的分配全落内部 SRAM**。想让文字进 PSRAM **必须显式 `heap_caps_malloc`** |
| 内存账（为什么现在不用 PSRAM 也没事） | 一条识别结果通常 20~40 汉字 = 60~120 字节（`BUS_TEXT_LEN=2048` 是极端上限）；10 条 ≈ **1~2 KB 内部 SRAM**，可忽略。真正需要 PSRAM 的是**LLM 长回复**（每条可上千字节 → 10 条 20KB） |

### 6-G 55 秒单轮上限（网关协议要求）

| 事实 | 含义 |
|---|---|
| 网关规定**单轮最长 55 秒**（`APIserver.md` §2），超出回 `{"type":"error","code":3}` | 到点后网关已停止识别，再发音频全是白费 |
| 板子侧 `VOICE_MAX_RECORD_MS = 50000`（`business/voice.cpp`），取 50 秒留 5 秒余量 | 网关的计数起点是它**收到 `start`** 的时刻，可能比我们早几十 ms，卡 55000 有踩线风险 |
| 到点**走正常收尾**（`mic.stop` + `asr.end` + `ENDED`），**不是 abort** | 这样服务器的 `final` 还能回来，用户拿到的是完整识别结果 |
| **兜底链路** | `ws.cpp` 的 `error` 分支**不再 `return`**（继续走查表分发）→ `UiBridge::on_ws_error` 解析 code → **1(讯飞错误) / 3(超时) 投 `VOICE_STOP`** 让采音收尾（用 `m_asr_active` 守卫，不在录音就不投）。2(llm失败)/4(未知服务) 只记日志 |
| ★ **配套（不加就会出幽灵 bug）** | `UiBridge::voice_start()` **先清空 `voice_q` 再投 START** —— 否则 `on_ws_error` 投的 STOP 若恰好残留在队列里，会被下一轮开头的 `voice_q_drain` 当成"用户松手"→ **新会话刚按下去就被取消**，且日志看起来莫名其妙 |

### 6-H 音频链路实测基准（**健康值，以后拿它对比**）

```
I (x) voice: level=314 | 本秒 发送=25 读失败=0 帧 (满帧应为 25), 读耗时均=34ms | GPIO中断累计=1
I (x) voice: 会话结束 (正常), 共发 284 帧 (=11.4 秒音频), 读失败丢弃 0 帧 (=0.0 秒)
```

| 数字 | 健康值 | 判读 |
|---|---|---|
| **发送帧/秒** | **25~26**（满帧 = `1000/40`） | **明显偏少 → 正在丢音频**（整轮 >40ms/帧 → DMA 缓冲积压绕圈 → 音频有洞） |
| **读失败帧** | **0** | 每多 1 就是 **40ms 音频被直接扔掉** |
| **读耗时均** | **34~37ms** | 生产恒定 40ms/帧，本循环每轮 = 读 + 发。读≈35ms → 发送只花 ~5ms，**余量充足**；读≈0ms → 发送吃掉 ~40ms，刚好卡平，一点抖动就积压 |
| **GPIO中断累计** | 一次按键涨 **几次~几十** | 几百/几千 = **引脚在噪声里翻转**（先查接线/接地，见 §5.0-A） |

> ★ 盲区提醒：**DMA 积压时 `read_frame` 依然会成功**（返回的是缓冲里最老的数据），
> 所以"发送帧数正常"**不能**单独证明没丢音频 —— 要**发送帧数 + 读耗时**一起看。

### 6-I 本轮修的其它 bug（源码级定位）

| 事实 | 含义 |
|---|---|
| 🐛 **`esp_websocket_client_send_*` 的 `timeout` 是 RTOS ticks，不是 ms**（`esp_websocket_client.h:279`） | 原 bug：把 `timeout_ms` 直接传进去 → `HZ=100` 时 `1000` 变成 **10 秒**。代价：音频发一帧最多阻塞 10 秒，而 I2S DMA 只有 **128ms** → 整段音频全丢。**修法**：`ws.cpp` 加 `ws_timeout_ticks()` 统一换算 + 下限夹到 **1 tick**（`pdMS_TO_TICKS(1..9)==0`，而组件里 `timeout=0` 表示"不等待"） |
| 🐛 **WS 回调里的 `printf`+`fflush(stdout)`** → 改为 `ESP_LOGD` | 两个后果实测都踩到：① 持 `client->lock` 打串口 → 拖住 `voice_task` 的 `send_audio`；② 往 UART 灌字符 → UART 中断频繁触发（那次 Interrupt WDT panic 的 `EPC1` **正是 `uart_hal_write_txfifo`**） |
| 🐛 **`esp_timer_restart` 从 ISR 里调【不轻】** → ISR 加"同 tick 限流" | 它做 `timer_list_lock()`（**屏蔽中断**）+ 64 位乘除 + **两次有序链表遍历**（`esp_timer.c:135-177`）。抖动时 ISR 反复重入 → CPU0 几乎 100% 泡在 ISR → 中断看门狗饿死 → panic。**修法**：`xTaskGetTickCountFromISR()` 判同 tick 只做一次，上限 100 次/秒（对 50ms 消抖完全够） |
| 🐛 **`button_edge_init` 里 `gpio_config()` 之后立刻采初值** → 连采到"连续 10 次不变"（最多 100ms） | 引脚那一刻还没稳 → 读到低 → `last_settled` 记成"按下" → 等它稳下来触发一次边沿 → **误报一条"释放"**。实测：开机 2.4s、用户没碰按键就冒出 `[按键] 录音键 松开 → voice_stop` |
| 🐛 **`handle_data` 把每条消息拷到 1KB 栈缓冲，超出就截断** → 改成**在 `m_rx_buf` 里就地分发** | 截断后 `cJSON_Parse` 失败 → **整条消息被静默丢弃**。V2 下 LLM 的 `reply` 可带 2KB 正文 → 必炸。做法：临时把下一个字节改 `\0`、分发完改回（`cJSON_Parse` 内部会复制，安全）。**副产品：给 `websocket_task` 的 4K 栈省掉 1KB** |
| 🐛 **`voice_session` 的"等连接"循环（最长 15 秒）不消费 `voice_q`** → 抽 `voice_q_drain()` 并在所有等待循环里调用 | 期间 `voice_task` 是 `voice_q` 唯一消费者，一不消费：① 深度只有 4，按几次就满；② **`VOICE_STOP` 也一起丢 → 用户松手被忽略** → 连上后照常录一段用户不要的会话。实测踩过开机 2.4s 打出 `voice_q 满, VOICE_STOP 丢弃` |

### 已确认可用的重载/配置

- `ui_chat` 屏的控件树（**本轮用户重新设计过**，见 §6-F）：`ui_contextpanel`(flex column 容器)
  下挂**每轮新建**的 `roll`→`panel`→`label` 三层。**SquareLine 预置的 `ui_merollpanel`("你好") /
  `ui_resrollpane`("hello") 是模板，会被代码在进屏时删掉** —— 别以为屏幕上那两条是真实消息。
- `ui_TabView3`("choose model") 已加 `LV_OBJ_FLAG_HIDDEN`（隐藏对象不参与布局也不绘制）。
- 图片素材两类格式：**I8 索引**（图标/logo）与 **RGB565A8**（大图）。I8 需开 `CONFIG_LV_BIN_DECODER_RAM_LOAD=y` 才能显示（已开）。

---

## 7. 用户偏好与协作约束（**请遵守**）

1. **用户自己构建/烧录**（"我来构建"）。不要代为执行会污染 build 缓存的构建；改完代码交给用户编译。
2. **未说明的配置不要自行推测，直接问**。用户明确要求过这点。
3. **`main/apikey.h` 含讯飞等凭据，必须 gitignore，绝不提交。**
4. **LVGL 锁定 9.5**（SquareLine ESP-IDF 导出目标），不要升级。
5. **屏幕 bring-up 用编译期宏做旋钮**（`lcd_display.hpp`），用户习惯自主迭代调参。
6. **用户是 Arduino 背景**，IDF 特有 API 不熟。协作分工见 [`REBUILD.md`](REBUILD.md) 末节
   "🧑💻 你可以自己写的部分"——**纯业务逻辑留给用户写，IDF 重活（队列/任务/中断/indev/CMake）由 AI 写**。
7. **用中文交流**。

---

## 8. 文档索引

| 文件 | 内容 | 状态 |
|---|---|---|
| **`HANDOFF.md`** | ← 本文件，接手入口 | 最新 |
| ★★ [`PROTOCOL.md`](PROTOCOL.md) | **协议 V2 完整规范**（ASR 全量 `asr` / `partial` 归 LLM / 网关侧要改什么 / 内存账） | **新增，改协议前必读** |
| [`REBUILD.md`](REBUILD.md) | **重构完整方案**（双通道队列/任务布局/阶段/消息结构/4 大坑/分工） | ✅ **已同步到 V2** |
| `drivers/gpio_isr_once.hpp` | GPIO ISR 服务"只装一次"helper（消除启动日志里的误导性 `E`） | 新增 |
| [`APP_FSM.md`](APP_FSM.md) | **旧**状态机工作逻辑详解（`AppFsm` 的 tick 七步、WS 回调分发表） | 描述**重构前**的架构，供理解历史 |
| [`APIserver.md`](APIserver.md) | 服务器端 WS 协议说明（V1.5）| ⚠️ **已被 `PROTOCOL.md` 取代**，顶部有指向说明 |
| `problem.md` / `STACK_OVERFLOW.md` | 历史问题记录（栈溢出等） | 历史 |
| `README.md` | 项目说明 | 可能过时 |
| **`AGENTS.md`** | 项目指南（在仓库根，不在 `MD/`）| ⚠️ **信息过时**（称 v6.0.1/2MB flash/无 PSRAM/main.c 为空，且 §2 的按键引脚还是旧的），**以本文件 §2 为准** |

### 记忆文件（不在仓库，在用户机器上）

- `C:\Users\30709\.claude\projects\D--Desktop-ESP-IDF-Lanlink\memory\`
  - `psram-80mhz-unstable.md` —— PSRAM 80MHz 位翻转记录
    ⚠️ **该结论方向待重新评估**：本轮发现接地不良也能造成随机位翻转（见 §9 顶部）。

---

## 9. 已知风险 / 待办

- ⚠️ **PSRAM 80MHz 曾有位翻转记录**（旧记录称是音频错字根因），但当时屏幕引脚 35/36/37 正压着 PSRAM 数据线。
  重接线后雪花消失，故判断是**接线冲突**，当前已改回 80MHz。
  `CONFIG_SPIRAM_MEMTEST=y` 保留作开机自检兜底 —— **若启动 abort，说明该判断有误，需回退 40MHz**。
- ★★ **待重新评估：上面那条"PSRAM 位翻转"的结论方向可能是错的。**
  本轮发现**接地不良**（§5.0-A）能让整块板的地平面靠 GPIO 的 ESD 二极管维持 ——
  一个漂移的地平面同样能造成随机位翻转。**若当时也是接地问题，那这个锅就不该由 PSRAM 背。**
  下次再遇到位翻转/随机崩溃，**先量"板子 GND 引脚 ↔ 面包板 GND 轨"之间的电压**（应在几 mV 以内）。
- ⚠️ `main/CMakeLists.txt` 用 `SRC_DIRS` + glob：**每次新增源文件都要 touch 它**（见 §6）。
- 📌 **阶段 1 收尾已完成**：`diag_task` 与全部临时诊断接口（`encoder_isr_hits` / `encoder_peek_raw` /
  `button_edge_isr_hits` / `button_edge_timer_hits` / `lvgl_encoder_*`）已删除；
  `E gpio: GPIO isr service already installed` 噪音已消除（新增 `drivers/gpio_isr_once.hpp`）；
  `lcd_display.hpp` 过时引脚注释与无用的 `ENC_A_PIN` 宏已清理；
  `lvgl_port_send_encoder_dir()` 与 `encoder_dir_t` 死代码已删除。
  **输入链路若再出问题，诊断需重新添加**（照 §5.2 末尾的"教训"写）。
- 📌 **阶段 0 / 1 / 2 / 3 已完成并上机验证**。回归验证清单（下次改动后照此看串口）：
  1. 构建过；串口**无** `handler 表已满`（实测只需 3 个 handler 位，表深 4）
  2. `ui_bridge: 就绪: voice_q=4 resp_q=16 stream_q=1 (共约 3304 字节内部 SRAM)`
     ← **这个数是"消息结构没被改坏"的硬指标**，改 `resp_msg_t`/union/`BUS_TEXT_LEN*` 后必须对照。
     （V1 是 8932，V2 缩到 3304，见 §6-A）
  3. `ui_bridge: [连接] ... status=0`（`RESP_WS_STATUS` + `CONNECTED`）
  4. `ws_keeper: 网关已连接 (服务由 voice_task 在会话开头切换)`
  5. **阶段 1 无回归**：转编码器仍切屏、按键仍一次一沿、**无 Task WDT**
  6. 按**录音键（GPIO2）** → `[会话] status=3`(STARTED) → **聊天屏立刻冒出一个小绿气泡**
     → `voice: 服务器已确认切到 text` → `已发送 start` → `采音开始 (每帧 1280 字节 / 40ms)`
     → 说话 → `[流式] kind=0 final=0 text="..."` **逐步变长（全量替换，不重复叠加）**
     → 松开 → `[流式] ... final=1` 整句完整 → `[上屏] 定格: ... (PSRAM 空闲 XXXX KB)`
     → `[会话] status=4`(ENDED)
  7. **按下后立刻松开**（不说话）→ 小绿气泡出现后**自己消失**（空气泡被删）
  8. 按**服务键（GPIO42）** → 只有一条日志，**不投任何队列**
  9. 拔网线 → `status=1`(DISCONNECTED) → `status=2`(RECONNECTING) → 插回 → `status=0`(CONNECTED)
     **待实测**：拔网线录音 → ABORTED + `ws_keeper` 重连 → 再按可开新会话
  10. **快速点按**（按下即松）→ `voice: 等 svc_ok 期间用户已松手 (快速点按), 本轮取消`，不崩、不留残留会话
  11. **待实测**：按住录音键**超过 50 秒**不说话 → `已达单轮上限 50 秒, 自动收尾` → 仍能收到 `final`
  12. **待实测**：录音过程中转编码器 —— UI 是否仍不卡
  13. `voice: level=xxx | 本秒 发送=25 读失败=0 ...` —— 每秒一条，**健康值判读见 §6-H**；
      同时用于标定 `VOICE_SPEECH_LEVEL`
- 📌 **阶段 3 已完成**（真采音 + 上屏都做完了，见 §3 各阶段成果）。**遗留一项未接线**：
  "服务器无响应判定"（逻辑已写在 `voice.cpp` 注释里，只差 `VOICE_SPEECH_LEVEL` 阈值标定
  + `UiBridge::asr_last_ms()`）。
- 📌 **阶段 4 待做（清理加固）**：
  - `rtasr.cpp` 的 `printf` **已降级为 `ESP_LOGD`** ✓（本轮做完）
  - `queue` 是否改 `xQueueCreateStatic` + PSRAM（现在只有 3304 B，优先级已降低）
  - `UiBridge` 拆分（总线层 / 映射层）—— 上屏代码进来后 `ui_bridge.cpp` 变长了
  - **渲染提速**（见文末"UI 性能"）
  - 删留档代码（`app_fsm.*.txt` / `button.*.txt`）
  - ✅ **已做完**：`HANDOFF.md` / `REBUILD.md` / `PROTOCOL.md` 的 V2 同步
- 📌 **已知行为（未改，用户明确要求先不动）**：`lv_indev_read()` 在屏幕动画期间
  （`prev_scr != NULL`，约 500ms）会在调 `read_cb` **之前** return，所以动画期间转的跳变会
  **攒在 `s_accum` 里**，动画结束后一次结算 → `enc_diff` 可能是十几格 → `indev_encoder_proc`
  的 `for` 循环连发多次键 → **可能连跳好几屏**。
  若要治：在 `lvgl_encoder_read_cb` 里把每轮 `steps` 夹到 ±1（会牺牲"快转=快切"的手感）。
- 📌 **UI 性能**：FULL 双整屏缓冲(2×106KB PSRAM) + 40MHz SPI + loading 屏无限旋转动画
  → 实测每轮 `lv_timer_handler()` 约 **137ms（≈7 FPS）**，CPU1 基本被渲染吃满。
  不致命（WDT 已用 tick 夹紧修好），但要提速得改渲染模式（阶段 4）。

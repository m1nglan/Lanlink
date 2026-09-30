# Lanlink 交接文档（Handoff）

> 本文件是**接手本项目的入口**。先读这里，再按需读文末「文档索引」。
> 最后更新：**阶段 1 完成**（输入事件化已在硬件验证；待 commit）。下一步补阶段 0，然后进阶段 2。

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

| 用途 | 引脚 |
|---|---|
| 屏幕 SPI2 | SCLK=21, MOSI=20, RST=19, DC=47, CS=48, BL=45 |
| 编码器 | A=**7**, B=**15** |
| 物理键 | IO10=录音键, IO8=服务键（均为按下为低，内部上拉） |
| I2S 麦克风 | BCLK=11, WS=12, DIN=13 |

⚠️ **n16r8 的 Octal PSRAM 占用 GPIO33–37**（D4=33…DQS=37）→ 屏幕/外设**绝不能用 33/34/35/36/37**。
> 历史教训：屏幕旧引脚曾用 35/36/37，与 PSRAM 冲突导致雪花 + PSRAM 位翻转（见 §6）。

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
| 最新提交 | `763f5a2 修复抖动_阶段1完成` |
| 工作区 | 有未提交改动（阶段 1 收尾 + 阶段 0 + **阶段 2**，**已构建通过 + 已上机验证**） |
| 阶段 | **阶段 0 + 1 + 2 完成**；下一个是**阶段 3**（真采音闭环） |

### 各阶段成果

**阶段 1（已硬件验证）** —— 输入事件化：

- ✅ 转编码器 → LVGL indev → 切屏正常（**无独立任务、无跨任务锁**）
- ✅ 按 IO10 / IO8 → GPIO 中断 + esp_timer 消抖 → 边沿回调正常（**无轮询**）
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
- ✅ `ws_keeper_task`（连接生命周期 + 状态上报）、`voice_task`（命令驱动，**假会话不采音**）
- ✅ `main.cpp` 重写：WiFi + WS + 三任务 + 按键接线
- ✅ **实测通过**：冷启动建连 + 切 text；按 IO10 走完 `start → end`；
  `ws_keeper → resp_q → lvgl 任务` 整条跨任务链路打通（日志 `[结果] kind=2 ... status=0`）
- ✅ **队列内存实测 8932 字节**（`resp_msg_t` 524 × 16 + `stream_msg_t` 516 + `voice_cmd_msg_t` 8 × 4）
  —— 这个数字是"消息结构没被改坏"的硬指标，改结构后要对照

### 👉 下一步

进**阶段 3**（真采音闭环）：把 `voice_session` 的假会话换成真采音循环
（骨架已写在 `voice.cpp` 注释里，含三个必须注意的边界），并接 `stream_q` 流式上屏。

---

## 4. 架构与重构计划

**完整重构方案见仓库内 [`REBUILD.md`](REBUILD.md)**（就是当前正在执行的计划，含双通道队列设计、任务布局、分阶段步骤、消息结构）。

核心思路（一句话）：**业务从"状态机轮询"改为"事件/命令驱动"**，
`RtAsr`/`Llm`/`WS` 协议层几乎不动（干净无状态），重写的是调度层（`AppFsm` 退役）。

阶段划分：~~0 基础设施~~（**已补**）→ ~~1 输入事件化~~（**已硬件验证**）→ ~~2 ws_keeper+UiBridge~~（**已硬件验证**）→ **3 语音采音闭环（下一个）** → 4 清理。


### 阶段 2 的任务/上下文地图（**这是读代码的入口**）

| 任务 | 核 | prio | 栈 | 职责 |
|---|---|---|---|---|
| `lvgl` | 1 | 2 | 6K | 渲染 + 编码器 indev + **UI 队列消费 timer(50ms)** |
| `ws_keeper` | 0 | 5 | 8K | WS 连接生命周期：建连 / 重连 / ping 保活 / 切 text |
| `voice` | 0 | 6 | 6K | 命令驱动语音会话（阶段 2 是**假会话**，不采音） |
| `websocket_task` | 0 | 5 | 4K | **组件自带**：收包 → 同步跑 WS 回调（在 `ws.cpp` 钉核） |
| ~~按键 / 编码器~~ | — | — | — | **无任务**：GPIO 中断 + esp_timer / LVGL indev |

数据流（**跨任务只走队列；发送是同步函数调用**）：

```
按键(中断→esp_timer) --voice_q--> voice_task --asr.start/send_audio/end--> 服务器
服务器 --> websocket_task(回调) --resp_q/stream_q--> lvgl 任务(UI timer) --> 屏幕
```

> ⚠️ 阶段 2 往 **CPU0** 加了 3 个东西（`ws_keeper` prio5、`voice` prio6、组件 `websocket_task` prio5）。
> §6 那个 `pdMS_TO_TICKS` 陷阱已经从 CPU1 搬到 CPU0 —— 新任务一律用"夹紧 tick 值"的写法，
> 否则 IDLE0 会被饿死、Task WDT 照炸。（`drain_queues` 里也已把 1KB 消息体改成 `static` 给
> 6K 栈让位。）

---

## 5. 输入链路（阶段 1，**已全部解决**）

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
| **回调期间持有 `client->lock`**：主循环是 `xSemaphoreTakeRecursive(client->lock)` → `recv()` → 里面调 `dispatch_event()`（`esp_websocket_client.c:1397-1402` / `1118`） | ★★ **回调里绝对不能阻塞**。所以 `stream_q` 用 `xQueueOverwrite`（天生不阻塞），`resp_q` 必须 `xQueueSend(..., 0)`。**用 `portMAX_DELAY` 会让整个 WS 收发死锁**（TX/RX 共用这把锁）。另：回调里别做耗时 `printf` |
| `CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK` **未开** → `client->lock` 一把递归锁 TX/RX 共用（`esp_websocket_client.c:816`） | ✅ REBUILD 坑#2 属实 → `voice_task`/`ws_keeper`/`websocket_task` **必须同核 CPU0**。递归锁保证了**同任务重入不死锁** |
| `WS::set_handler()` 按 `type` **自带去重**；`WS::deinit()` **不清 handler 表** | → **重连不需要重新 `asr.attach()`**，handler 表挂在 WS 单例上会一直保留，`init()` 会把它们挂到新 client 的 event loop 上 |
| 组件配了 `disable_auto_reconnect = true` / `disable_pingpong_discon = true` | **重连和保活必须由我们的 `ws_keeper` 负责**，别指望组件 |

### 业务层铁律

| 事实 | 含义 |
|---|---|
| ★ **`partial` 的路由只认 `WS::m_service`**（`ws.cpp:313`），而 `RtAsr::switch_service()` **只发消息、不改 `m_service`**（`rtasr.cpp:42`） | 切服务必须**成对**：`ws.set_service("text")` **+** `asr.switch_service("text", ...)`。漏第一句 → 语音 partial 被送去 chat 槽位（阶段2 是 NULL）→ `if (h != NULL)` 无 else、不打日志 → **识别文字静默消失**。旧代码在 `app_fsm.cpp:125-126` 就是成对写的 |
| `RtAsr::handle_partial/final/revise` 三者只差"取到 text 之后干什么" | 已抽成 `extract_text()` + 三个薄壳（`rtasr.cpp`）。**别把 `revise` 并进 `final`**：`revise` 故意不触发 `m_cb`，合并会让 UI 收到假的"说完了" |
| `WS::dispatch_msg` 里 `partial` **不走查表**，因为 `type=="partial"` 被"语音"和"LLM"两个服务共用，一维 `type` 分不出来 | 见上一条 |

### 消息载荷的约定（`bus_msg.hpp`）★ 改动前务必先读

| 事实 | 含义 |
|---|---|
| `resp_msg_t` 的载荷是 **union**（`u.text[512]` / `u.i32` / `u.f32` / `u.sta`），**`kind` 决定读哪一项** | 消费端**必须**先看 `kind` 再取 `u` —— 拿状态类的消息去读 `u.text` 会打出乱码（`drain_queues` 里已按 kind 分支） |
| **union 成员必须是"平凡类型"，且不得含指针** | 队列靠 `memcpy` 搬字节，只保护这 512 字节本身。成员里若放了 `char*`，等于把值拷贝退回成"指针 + 一块无人保护的内存"（悬空/被改写）。要放字符串就用**内联定长数组** |
| union 大小 = **最大成员**（现为 `text[512]`）→ `resp_msg_t` 恒为 **524 字节** | 新增成员只要 ≤ 512，**队列内存不变**（实测总数 8932）。加了 8 字节对齐的类型（某些 ABI 下 `double`）会让结构体涨到 528 —— 加完请对照那行启动日志 |
| **状态统一走 `status_kind_t`**（CONNECTED/DISCONNECTED/RECONNECTING/STARTED/ENDED/ABORTED） | 不管哪个服务，报给 LVGL 的状态都写进这个枚举；投递用 `post_resp_status(kind, status)`。**标签仍按服务分**（`RESP_ASR_STATUS` / `RESP_WS_STATUS`）以保留"谁报的" |
| 三个投递接口最后都走 **`post_resp_msg()`** 入队 | 那是 `resp_q` 的**唯一入队点**：`timeout=0` 的硬要求和"满了告警"只写一份。加新载荷类型时，调用方自己填 `resp_msg_t` 再调 `post_resp_msg()` 即可，**不用改 UiBridge** |
| `voice_cmd_msg_t.arg` 是**标量参数槽位**（4 字节），当前**全部传 0、没人读** | 装不下文本；**更不要拿它塞指针**（32 位机上 `int32_t` 和指针同宽，编译通过但会引入悬空） |
| 状态上报要**去抖**（见 `ws_keeper.cpp::report_ws_status`） | 重连循环约 1 轮/秒，`resp_q` 深度只有 16 —— 无脑投递十几秒就满，开始丢消息（含 `RESP_ASR_FINAL`）。故只在**状态变化**时投一条；`DISCONNECTED` 只在"连上过又断了"时报，开机没连上不报 |

### 已确认可用的重载/配置

- `ui_chat` 屏**已有现成对话控件**：`ui_metext`（我说的，绿气泡）/ `ui_restext`（回复，深色气泡）——微信式布局已画好，等接数据。
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
| [`REBUILD.md`](REBUILD.md) | **重构完整方案**（双通道队列/任务布局/阶段/消息结构/4 大坑/分工） | 最新，当前计划 |
| `drivers/gpio_isr_once.hpp` | GPIO ISR 服务"只装一次"helper（消除启动日志里的误导性 `E`） | 新增 |
| [`APP_FSM.md`](APP_FSM.md) | **旧**状态机工作逻辑详解（`AppFsm` 的 tick 七步、WS 回调分发表） | 描述**重构前**的架构，供理解历史 |
| [`APIserver.md`](APIserver.md) | 服务器端 WS 协议说明 | 参考 |
| `problem.md` / `STACK_OVERFLOW.md` | 历史问题记录（栈溢出等） | 历史 |
| `README.md` | 项目说明 | 可能过时 |
| **`AGENTS.md`** | 项目指南 | ⚠️ **信息过时**（称 v6.0.1/2MB flash/无 PSRAM/main.c 为空），**以本文件 §2 为准** |

### 记忆文件（不在仓库，在用户机器上）

- `C:\Users\30709\.claude\projects\D--Desktop-ESP-IDF-Lanlink\memory\`
  - `psram-80mhz-unstable.md` —— PSRAM 80MHz 位翻转记录（见下方风险）

---

## 9. 已知风险 / 待办

- ⚠️ **PSRAM 80MHz 曾有位翻转记录**（旧记录称是音频错字根因），但当时屏幕引脚 35/36/37 正压着 PSRAM 数据线。
  重接线后雪花消失，故判断是**接线冲突**，当前已改回 80MHz。
  `CONFIG_SPIRAM_MEMTEST=y` 保留作开机自检兜底 —— **若启动 abort，说明该判断有误，需回退 40MHz**。
- ⚠️ `main/CMakeLists.txt` 用 `SRC_DIRS` + glob：**每次新增源文件都要 touch 它**（见 §6）。
- 📌 **阶段 1 收尾已完成**：`diag_task` 与全部临时诊断接口（`encoder_isr_hits` / `encoder_peek_raw` /
  `button_edge_isr_hits` / `button_edge_timer_hits` / `lvgl_encoder_*`）已删除；
  `E gpio: GPIO isr service already installed` 噪音已消除（新增 `drivers/gpio_isr_once.hpp`）；
  `lcd_display.hpp` 过时引脚注释与无用的 `ENC_A_PIN` 宏已清理；
  `lvgl_port_send_encoder_dir()` 与 `encoder_dir_t` 死代码已删除。
  **输入链路若再出问题，诊断需重新添加**（照 §5.2 末尾的"教训"写）。
- 📌 **阶段 0 / 1 / 2 已完成并上机验证**（构建通过；建连+切 text；IO10 走完 start→end；
  跨任务队列链路打通）。回归验证清单（下次改动后照此看串口）：
  1. 构建过；串口**无** `handler 表已满`（实测只需 3 个 handler 位，表深 4）
  2. `ui_bridge: 就绪: voice_q=4 resp_q=16 stream_q=1 (共约 8932 字节内部 SRAM)`
     ← **8932 这个数是"消息结构没被改坏"的硬指标**，改 `resp_msg_t`/union 后必须对照
  3. `ui_bridge: [结果] kind=2 ... status=0`（`RESP_WS_STATUS` + `CONNECTED`）
  4. `ws_keeper: 网关已连接, 已切到 text 服务`（服务器回 `svc_ok`）
  5. **阶段 1 无回归**：转编码器仍切屏、按键仍一次一沿、**无 Task WDT**
  6. 按 IO10 → `status=3`(STARTED) → `voice: 已发送 start` → 1s 后 `已发送 end` → `status=4`(ENDED)
  7. 按 IO8 → **只有** `[按键] 服务键 按下 (IO8 未接业务)`，**不投任何队列**
  8. 拔网线 → `status=1`(DISCONNECTED) → `status=2`(RECONNECTING) → 插回 → `status=0`(CONNECTED)
- 📌 **阶段 3 待做**：`voice_session` 的假会话换成真采音循环（骨架已写在 `voice.cpp` 注释里，
  含三个必须注意的边界：读失败不能发包 / STOP 检查别被 `continue` 跳过 / 发送阻塞会吃 128ms DMA 余量）。
  同时把 `drain_queues()` 里的 log 换成真正的 `switch (r.kind)` 上屏（那个 if/else 就是雏形）。
- 📌 **阶段 4 待做（清理）**：`rtasr.cpp` 里 `printf` 在 WS 回调(持 `client->lock`)里跑，
  网络抖动时可能拖住发音频 → 降级为 `ESP_LOGD`；`queue` 是否改 `xQueueCreateStatic` + PSRAM。
- 📌 **已知行为（未改，用户明确要求先不动）**：`lv_indev_read()` 在屏幕动画期间
  （`prev_scr != NULL`，约 500ms）会在调 `read_cb` **之前** return，所以动画期间转的跳变会
  **攒在 `s_accum` 里**，动画结束后一次结算 → `enc_diff` 可能是十几格 → `indev_encoder_proc`
  的 `for` 循环连发多次键 → **可能连跳好几屏**。
  若要治：在 `lvgl_encoder_read_cb` 里把每轮 `steps` 夹到 ±1（会牺牲"快转=快切"的手感）。
- 📌 **UI 性能**：FULL 双整屏缓冲(2×106KB PSRAM) + 40MHz SPI + loading 屏无限旋转动画
  → 实测每轮 `lv_timer_handler()` 约 **137ms（≈7 FPS）**，CPU1 基本被渲染吃满。
  不致命（WDT 已用 tick 夹紧修好），但要提速得改渲染模式（阶段 4）。

# Lanlink 交接文档（Handoff）

> 本文件是**接手本项目的入口**。先读这里，再按需读文末「文档索引」。
> 最后更新：阶段 1 实施中途（正在排查输入链路）。

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
| 最新提交 | `080b442 GPIO改esp中断` |
| 工作区 | 干净（**但已加了未提交的诊断代码，见 §5**） |
| 阶段 | **重构阶段 1 实施中**（输入事件化），卡在"输入无反应" |

### 🚨 立即可见的问题（接手第一件事）

`main/main.cpp` 现在是**阶段 1 最小验证版**：只初始化 LVGL + 编码器 + 按键 + 诊断任务，
**没有 WiFi / WS / 语音 / LLM**（这些是阶段 2+ 的事）。

用户实测反馈：**按物理键、转编码器，屏幕和串口均无反应。**

**进展**：
- 编码器那条链的根因**已在源码级定位并修复**（编辑模式被 `lv_group_focus_obj` 清掉，见 §5）。
- 按键那条链的驱动逻辑已逐行核对无误，怀疑接线/电平，等诊断日志。
- **当前待办：烧录一次，按 §5 的判读表读日志。**（诊断代码 + 修复都在工作区，未提交、未烧录）

---

## 4. 架构与重构计划

**完整重构方案见仓库内 [`REBUILD.md`](REBUILD.md)**（就是当前正在执行的计划，含双通道队列设计、任务布局、分阶段步骤、消息结构）。

核心思路（一句话）：**业务从"状态机轮询"改为"事件/命令驱动"**，
`RtAsr`/`Llm`/`WS` 协议层几乎不动（干净无状态），重写的是调度层（`AppFsm` 退役）。

阶段划分：0 基础设施 → **1 输入事件化(进行中)** → 2 ws_keeper+UiBridge → 3 语音采音闭环 → 4 清理。

---

## 5. 输入链路无反应

### ✅ 编码器无反应 —— **已定位（源码级），已修复**

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

**为什么旧方案没这问题**：退役的 `lvgl_port_send_encoder_dir()` 直接调 `lv_group_send_data()`，
绕开了编辑模式判断。改成 indev 后依赖编辑模式，就踩上了。

**修复**（`main/display/lvgl_port.cpp::lvgl_focus_active_screen_locked`）：
- 屏幕一变就 `lv_group_remove_all_objs()` 再 `add_obj(当前屏)` → 组里**只留当前屏**，
  消灭"旧屏残留导致每轮都走 focus_obj"的坑；
- 最后**无条件重申** `lv_group_set_editing(g, true)`（函数内部幂等，`editing` 相同直接 return）→ 自愈。
- 稳定态下两个条件都成立 → 整块跳过，**零额外开销、无额外重绘**。

### 物理按键无反应 —— **尚未定位，等烧录诊断**

`button_edge` 的 ISR/消抖逻辑已逐行核对，**是对的**：
- `esp_timer_restart()` 未 arm 时返回 `ESP_ERR_INVALID_STATE` 且不动它
  （`esp_timer.c:143-145`，`timer_armed()` = `alarm > 0`，`esp_timer.c:373`）→ ISR 里回退
  `esp_timer_start_once()` 覆盖首次边沿；一次性 timer 到期后 `alarm` 归零（`esp_timer.c:426`），
  下次边沿同样走回退分支。✓
- GPIO 中断只装在一个核上（`gpio_intr_enable_on_core(..., esp_intr_get_cpu(handle))`，
  `esp_driver_gpio/src/gpio.c:572`），app_main 在 CPU0，无亲和性坑。✓

→ 所以按键更像是**接线/电平**问题，诊断行会给答案。

### 诊断（已加，**未烧录验证**）

| 文件 | 新增接口 |
|---|---|
| `main/drivers/encoder.{hpp,cpp}` | `encoder_isr_hits()`、`encoder_peek_raw()` |
| `main/drivers/button_edge.{hpp,cpp}` | `button_edge_isr_hits()`、`button_edge_timer_hits()` |
| `main/display/lvgl_port.{hpp,cpp}` | `lvgl_encoder_read_calls()`、`lvgl_encoder_steps_total()`、`lvgl_encoder_editing()`、`lvgl_encoder_focus_ok()` |
| `main/main.cpp` | `diag_task`：每 500ms 打印全部计数 + 引脚裸电平 + 组状态 |

串口每 500ms 输出：
```
[诊断] IO7=1 IO15=1 | IO10=1 IO8=1 | enc_isr=0 enc_raw=0 | btn_isr=0 btn_tmr=0 | indev_read=1234 enc_steps=0 edit=1 focus=1
```

**按键（按住 IO10 时）：**
| 现象 | 结论 |
|---|---|
| 完全没有 `[裸电平]`，且 `曾低过=0/0` | **你拉的那个脚不是 GPIO10/GPIO8**（排针丝印认错/焊错点/焊盘坏），软件无辜 |
| 有 `[裸电平]` 或 `曾低过=1`，但 `btn_isr` 不涨 | GPIO 中断没触发 |
| `btn_isr` 涨但 `btn_tmr` 不涨 | esp_timer 没跑 |
| 都涨但无 `[btn_edge]`/`[按键]` 日志 | 消抖后电平判断反了（如上电瞬间脚就是低的） |
| 一次按下打出多条 按下/释放 | **消抖时长不够** → 见下方 §5.3 |

#### 5.3 按键抖动（2026-xx 已处理）

阶段 1 初版 `BTN_EDGE_DEBOUNCE_MS = 15`，实测一次按下会打出多条 `按下/释放`。
**回归原因**：旧轮询驱动 `button.hpp` 用的是 `BTN_DEBOUNCE_MS(10) x BTN_DEBOUNCE_N(5)`
= **50ms 连续稳定**，那才是本硬件上验证过（含提交 `6fa1fc5「修复io8抖动」`）的值。
→ 已把 `BTN_EDGE_DEBOUNCE_MS` 改回 **50**（重启式消抖，等价于旧版"连续稳定 50ms"）。

权衡：按下延迟 ≈ 本值；松开→voice_stop 最坏 ≈ 本值 + 帧边界 40ms（阶段 3）。
若 50ms 仍抖，先怀疑**电气**（内部上拉只有 ~45kΩ，长线 + 40MHz SPI 串扰）：
外挂 10k 上拉 + 100nF 到 GND，而不是继续加大消抖。

**编码器：**
| 现象 | 结论 |
|---|---|
| `indev_read` 不动 | LVGL 没轮询该 indev（或屏幕动画一直没结束，`prev_scr` 卡住） |
| `enc_isr` 不涨 | GPIO 中断没触发 → 接线/编码器公共端 |
| `enc_isr` 涨但 `enc_steps` 不涨 | 跳变没凑够一格 → `ENC_KEY_STEP` 偏大（或编码器是无 detent 型） |
| `enc_steps` 涨但 `edit=0` | **就是上面那个 bug**（本版本已修） |
| `edit=1` 但 `focus=0` | 聚焦对象不是当前屏 |
| 四项都好但屏幕不动 | 才轮到怀疑 SquareLine 事件/屏幕本身 |

### 已排除的假设（省得重查）

- ❌ "indev 没绑 display" —— **错**。`lv_indev_create()` 内部已 `indev->disp = lv_display_get_default()`（`lv_indev.c:134`）。
- ❌ "`button_edge.cpp` 没被编译" —— 那是**上一轮**的 CMake glob 问题，已 touch CMakeLists 解决，`.obj` 已生成。
- ❌ "`lv_group_focus_obj` 清编辑模式的路径不成立" —— 注册那一刻确实逃过去了（`lv_group_add_obj` 内部
  refocus 走 `focus_next_core` 直接改 `obj_focus`），**是切屏之后才踩上的**。别只查注册路径。

### 两个"设计上本就无反应"的点

1. **启动屏 `ui_main` 不处理按键**：只处理 `SCREEN_LOADED`，然后**延迟 7000ms** 才切到 `ui_home`
   （`ui_home` 才处理 `LV_KEY_LEFT/RIGHT`）。→ **开机前 ~7.5 秒转编码器本就不该有反应。**
2. **屏幕动画期间输入被屏蔽**：`lv_indev_read()` 开头 `if(indev->disp->prev_scr != NULL) return;`
   （`lv_indev.c:244`）—— 注意这一句在 `read_cb` **之前**，所以动画期间 `indev_read`/`enc_steps`
   计数也会一起冻住，别误判成"indev 没被轮询"。

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
| 转动 → 焦点对象收到 `LV_EVENT_KEY`；按 ENTER 松开 → `LV_EVENT_CLICKED` | 做"按钮触发"就挂 `LV_EVENT_CLICKED` |
| **`lv_screen_active()` 是聚焦对象**才能收到键 | SquareLine 把切屏事件挂在**屏幕对象**上 |

### IDF / FreeRTOS

| 事实 | 含义 |
|---|---|
| `esp_timer_restart()` 对**未启动**的 timer 返回 `INVALID_STATE` 且**不启动它** | → ISR 里首次边沿须用 `start_once`，之后才 `restart` |
| `esp_timer_restart/start_once` 是 `ESP_TIMER_IRAM_ATTR` | ISR 内可安全调用 |
| `_Atomic` 是 C11，**C++ 不认**（IDF 用 `gnu++26`） | 用 `std::atomic` 或 `portMUX_TYPE` 临界区 |
| **C++20 起 `volatile` 的 `++` / `--` / 复合赋值（`+=` 等）被弃用**，IDF 用 `gnu++26`，默认 `-Werror=volatile` 会**直接编译失败** | 已在 `main/CMakeLists.txt` 加 `-Wno-error=volatile` **降级为警告**（与已有三条同一个 `target_compile_options` 块）→ 代码可继续写 `s_x++`。若哪天真要清零警告，改成 `x = x + 1;` 即可（`x += 1` 同样被弃用） |
| `std::atomic` 的 `fetch_add` 在 Xtensa 不保证无锁 | **ISR 内用 `portENTER_CRITICAL_ISR` 更稳** |
| **`CONFIG_FREERTOS_HZ=100`（本项目实测值）→ 1 tick = 10ms，`pdMS_TO_TICKS(1..9) == 0`** | **`vTaskDelay(0)` 只 yield，不让 CPU 给更低优先级任务**。任何"夹紧到 ≥5ms 再 `pdMS_TO_TICKS`"的写法在 HZ=100 下等于**没有让出**。必须夹紧 **tick 值**：`TickType_t t = pdMS_TO_TICKS(ms); if (t < 1) t = 1;`。踩坑现场：`lvgl_port_task` 因此 100% 占住 CPU1 → IDLE1 饿死 → Task WDT(5s) 触发 |
| `spi_bus_dma_memory_alloc(host,sz,caps)` 支持 `MALLOC_CAP_SPIRAM` | SPI2 + GDMA 可直读 PSRAM 绘图缓冲（自动 cache sync） |
| **`SRC_DIRS` + `file(GLOB)` 只在 CMake 配置阶段扫描** | **新增源文件后必须 `touch main/CMakeLists.txt`**，否则新文件不参与编译→链接期 undefined reference |

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
- 📌 阶段 1 的诊断代码是**临时的**，输入链路修好后应连同诊断接口一并删除。
- 📌 重构未完成：`main/business/` 目录（ui_bridge / voice / ws_keeper / bus_msg.hpp）**尚未创建**。

# Lanlink 项目指南 (ESP32-S3 / ESP-IDF)

> **接手项目请先读 [`MD/HANDOFF.md`](MD/HANDOFF.md)** —— 那里有当前状态、正在排查的问题、源码级验证过的坑。
> 协议细节看 [`MD/PROTOCOL.md`](MD/PROTOCOL.md)。本文件只放长期有效的环境与约定。

## 项目概述

基于 **ESP-IDF v6.0.2** 的 ESP32-S3 语音助手设备（名为 `Lanlink`）：
INMP441 麦克风 → I2S → WiFi → WebSocket → 服务器（讯飞 RTASR / LLM）→ ST7789 条屏 + LVGL 9.5 UI。

与用户交流请使用**中文**。

## 关键环境

| 项 | 值 |
|---|---|
| 目标芯片 | `esp32s3`（双核 Xtensa LX7），**n16r8** = 16MB flash + 8MB Octal PSRAM |
| SDK | IDF **v6.0.2**（`D:/esp/.espressif/v6.0.2/esp-idf`） |
| LVGL | **9.5.0**（registry 组件，**锁定**，SquareLine 导出目标，勿升级） |
| Flash | 16MB，自定义分区表 `partitions_custom.csv` |
| PSRAM | Octal **80MHz** 已启用（有历史位翻转记录，见 HANDOFF §9） |
| 构建/烧录 | **用户自行执行**，不要代为 build/flash |

### 引脚（★★ 以代码为准 —— 排查硬件前必须先对一遍代码）

> ⚠️ **本节已于最近一次接线变更后按代码逐项核实。**
> **原始定义位置见每行末尾** —— 遇到"引脚行为诡异"（上电拉低 / 误触发 / 无反应），
> **第一步就是打开这些头文件对一遍实际连线**，别信记忆、别信旧文档。
> 一键列全部引脚：在 `main/` 里搜 `GPIO_NUM_`。

| 外设 | 引脚 | 定义位置 |
|---|---|---|
| **ST7789 屏**（SPI2）| SCLK **9** ｜ MOSI **46** ｜ RST **3** ｜ DC **8** ｜ CS **18** ｜ BL **17** | `main/display/lcd_display.hpp:21-26` |
| **旋转编码器** | A **45**（"左"）｜ B **41**（"右"）| `main/drivers/encoder.hpp:22-23` |
| **物理按键** | 录音 **42** ｜ 服务 **2**（均**按下为低、内部上拉**）| `main/main.cpp:49-50` |
| **INMP441 麦克风**（I2S）| BCLK **21** ｜ WS **47** ｜ DIN **1** | `main/drivers/i2s_mic.hpp:29-31` |

⚠️ **n16r8 的 Octal PSRAM 占 GPIO33–37，外设绝不能用这几个脚。**（上表没用到 ✓）

⚠️ **用到了几个"特殊脚"，合法但必须知道：**

| 脚 | 特殊之处 | 用在哪 |
|---|---|---|
| **GPIO45 / GPIO46** | **strapping 脚**（VDD_SPI 电压 / ROM 打印）| 编码器 A、屏 MOSI |
| **GPIO3** | **strapping 脚**（JTAG 源选择）| 屏 RST |
| **GPIO39–42** | **JTAG 默认功能**（MTCK/MTDO/MTDI/MTMS）| 编码器 B=41、录音键=42 |

- 当普通 GPIO 用**没问题**（前提是**不接 JTAG 调试**，且别在启动早期依赖它们的电平）
- **但它们上电瞬间有默认电平** → 将来若再遇到"**开机头几秒引脚状态异常**"（拉低 / 误报按下 / ISR 风暴），**先怀疑这几个**
- 📌 历史上踩过一次"开机 4 秒内 3.3V 引脚被拉低 → 按键误报松开 + ISR 风暴"，**真根因是接地**（不是这些脚），见 [`MD/HANDOFF.md`](MD/HANDOFF.md) §5.0-A

## 构建与运行

用户使用 VS Code 的 ESP-IDF 扩展命令，或终端：
```bash
idf.py build        # 构建
idf.py flash monitor
```

⚠️ **新增源文件后必须刷新 `main/CMakeLists.txt` 的时间戳**：`SRC_DIRS` 用 `file(GLOB)` 只在 CMake
配置阶段扫描，不刷新则新文件不参与编译，报链接期 `undefined reference`。

```powershell
(Get-Item main\CMakeLists.txt).LastWriteTime = Get-Date    # ← Windows 上要用这个
```

⚠️ **`touch` 在这台机器（Windows PowerShell）上不存在** —— 照着 Unix 的写法敲会报"无法识别 touch"。

## 代码结构与约定

- 应用代码在 `main/`，由 `main/CMakeLists.txt` 的 `idf_component_register()` 注册
  （用 `SRC_DIRS` 自动扫描子目录；新增目录要加进 `SRC_DIRS`/`INCLUDE_DIRS`）
- 驱动放 `main/drivers/`，显示相关放 `main/display/`，UI 为 SquareLine 导出的 `main/lvgl/`
- 入口 `app_main(void)`；用 `ESP_LOGI/ESP_LOGE` 打日志
- **`main/apikey.h` 含凭据，必须 gitignore，绝不提交**

## ESP-IDF 常见坑（务必注意）

1. **FreeRTOS API 需显式包含头文件**，否则 `implicit declaration`：
   `freertos/FreeRTOS.h` + `freertos/task.h`
2. **C/C++ 混用**：`.cpp` 调 `.c` 导出函数时头文件需 `extern "C" {}`
3. **ISR 上下文禁用阻塞/延时 API**；任务间通信用队列/信号量
4. `pdMS_TO_TICKS(x)` 注意 `CONFIG_FREERTOS_HZ`
5. 多核：任务默认不绑核，需固定用亲和性参数（`xTaskCreatePinnedToCore`）
6. **`_Atomic` 是 C11，C++ 不认** —— 用 `std::atomic` 或 `portMUX_TYPE` 临界区；
   且 ISR 内优先用 `portENTER_CRITICAL_ISR`（`std::atomic` 在 Xtensa 不保证无锁）
7. `sdkconfig` 已被 `.gitignore` 忽略，不入库；改配置需重新构建
8. **★ SquareLine 导出的中文字体可能是【压缩】的** —— 看 `main/lvgl/fonts/ui_font_*.c` 里的
   `.bitmap_format`：`1` = 压缩，需要 `CONFIG_LV_USE_FONT_COMPRESSED=y`（**已在 `sdkconfig` +
   `sdkconfig.defaults` 里开着**）；`0` = 明文，不需要。
   ⚠️ **症状很坑**：字体压缩但没开解压器时，LVGL 的**度量**照常读（`glyph_dsc`）、**像素**直接
   `return NULL`（`lv_font_fmt_txt.c`）→ **框的长度对、字一个都不画**。
   ⚠️ 文件头 `Opts:` 里的 `--no-compress` **是假的**，**以 `.bitmap_format` 为准**。
   详见 [`MD/HANDOFF.md`](MD/HANDOFF.md) §6-C。

9. **★★ `main/lvgl/screens/ui_main.c` 有一处【手工改动】，重新导出 SquareLine 会还原** ——
   开屏屏 `LV_EVENT_SCREEN_LOADED` 里原来那句
   ```c
   _ui_screen_change(&ui_home, LV_SCREEN_LOAD_ANIM_FADE_ON, 500, **7000**, &ui_home_screen_init);
   ```
   **被删掉了**。换屏改由 `main/display/lvgl_port.cpp` 的**启动门**（`lvgl_port_boot_gate_start`）独占：
   等 WiFi / 网关握手 / NTP 三项就绪 **且** 开屏动画播完（≥4s）才切，最迟 20s 强制切。

   **⚠️ 重新导出后必须再删一次这句**，否则：
   - `lv_screen_load_anim()` 会设 `d->scr_to_load = ui_home`，只有动画的
     `start_cb`/`completed_cb` 会清它；
   - 之后启动门调 `_ui_screen_change(&ui_home, …)` 会被
     `if(act_scr == new_scr || d->scr_to_load == new_scr) return;` **静默吞掉**
     → **日志打印"切主界面"但屏幕纹丝不动，永久卡在开屏**；
   - 想提前撤销那个延迟加载只能用 `lv_anim_delete(ui_home, NULL)`，但它会让两个回调都不跑、
     `scr_to_load` **永久悬空** → 后果更糟（实测会偶发 `LoadProhibited` panic 在
     `lv_event_mark_deleted`）。

   **入场动画（`loading_Animation` / `*show_Animation`）不要删** —— 它们是独立的，删了就没动画了。

10. **★ 调 LVGL 的函数必须区分【建立时】和【回调时】** ——
    `lv_timer_create()` 本身是 LVGL 调用，**从别的任务调必须持 `lvgl_port_lock()`**；
    而 `lv_timer` 的**回调**是在 `lv_timer_handler()` 里跑的，那时锁已持有，**回调里绝不能再加锁**（会自死锁）。
    ⚠️ 不持锁从 `app_main` 里建 timer / 改 label，会踩着正在跑 `lv_timer_handler` 的 LVGL 任务 →
    内部状态（事件链表 / 堆）被破坏 → **几秒后在完全无关的地方崩**（例如删 label 时崩在
    `lv_event_mark_deleted`）。正确写法见 `main/main.cpp` 里 `start_ui_timer` 那段。

11. **★★ LVGL 自带的 `sprintf` 不支持 `%f`** —— 小数上屏必须用 libc 的 `snprintf`
    ```c
    // ❌ 屏幕上只剩一个字面量 'f'
    lv_label_set_text_fmt(ui_balancenum, "%.2f", 11.96);

    // ✅ 先用 libc 格式化到缓冲区，再交给 LVGL
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", 11.96);   // <stdio.h>，支持 %f
    lv_label_set_text(ui_balancenum, buf);
    ```
    **原因**：`lv_label_set_text_fmt` 走 LVGL 自己的 `lv_snprintf`，它**默认不编译浮点支持**
    （`sdkconfig` 里没有 `LV_SPRINTF_USE_FLOAT`）→ `%f` 不被识别，`f` 被当字面量。
    **症状极具误导性**：不报错、不崩，屏幕上就一个 `f`。
    ⚠️ `%d` / `%s` 是安全的，**只有浮点要注意**。见 `main/display/balance_view.cpp:34`。

12. **★★ "定时刷新"的控件绝对不要用 `LV_ANIM_ON`** —— 会让 LVGL 的动画链表无限增长
    ```c
    // ❌ 每 2 秒 append 一条动画，十几分钟攒上千条
    lv_bar_set_value(bar, pct, LV_ANIM_ON);

    // ✅ 定时刷新的东西不需要过渡
    lv_bar_set_value(bar, pct, LV_ANIM_OFF);
    ```
    **原因**：`lv_bar_set_value(..., LV_ANIM_ON)` 每次都 `lv_anim_start()` **追加**一条新动画，
    而 **LVGL 不会替换同对象上的旧动画**。`lv_timer_handler()` 每轮要遍历整个动画链表 →
    **越用越卡，最后整页卡死**。
    **症状很典型**：**停在某屏久了卡死，转走再回来又好了**（切走 → 数据停了 → 动画跑完被删 → 链表空了）。
    📌 实测发生在 Server 屏（网关每 2 秒推 `usage` × 3 个进度条）。
    **判据**：`LV_ANIM_ON` 只给"用户动作触发的一次性动画"（面板开合、焦点切换）；
    **任何由定时器 / 数据推送驱动的刷新一律 `LV_ANIM_OFF`**。见 `main/display/server_view.cpp`。

13. **★★ `lv_obj` 的事件默认【不冒泡】** —— 想"子对象没消费就交给父对象"必须显式开 flag
    ```c
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);   // 缺这一句，父对象永远收不到
    ```
    **原因**：LVGL 把键盘/编码器事件发给**聚焦对象**后，如果该对象没消费，事件**到此为止** ✗
    父链上的屏（SquareLine 的 `ui_event_*` 回调）**收不到**。
    📌 实测用在聊天屏：`ui_contextPanel`（可滚动）先消费 `LV_KEY_LEFT/RIGHT` 做上下翻，
    **滚到头时故意 `return` 不消费 → 事件冒泡到 `ui_chat` → 触发切屏**。
    没开这个 flag 时表现是"**滚到底就再也切不了屏，卡死在那**" ✗
    **配套注意**：没到头时要用 `lv_event_stop_bubbling(e)` 主动拦住，
    否则会"边滚动边切屏"。见 `main/display/chat_view.cpp` 的 `ctx_key_cb`。

14. **★ `lv_anim_delete()` 的第二个参数不能传 SquareLine 生成的回调**
    ```c
    // ❌ 类型不匹配：SquareLine 的回调是 void(*)(lv_anim_t*, int32_t)
    //    而 lv_anim_exec_xcb_t 是 void(*)(void*, int32_t)
    lv_anim_delete(obj, _ui_anim_callback_set_width);

    // ✅ 传 NULL 表示"这个对象上的所有动画"
    lv_anim_delete(obj, NULL);
    ```
    **原因**：SquareLine 生成的 `_ui_anim_callback_*` 签名是 `void(*)(lv_anim_t *, int32_t)`
    （因为要先从 `lv_anim_t` 里取 `user_data` 才能拿到真正的目标对象），
    **和 `lv_anim_exec_xcb_t` 不兼容** ✗ C++ 不会隐式转换。
    ⚠️ 传 `NULL` 的副作用：会删掉该对象上**所有**动画 —— 用之前确认上面没有别的动画。
    见 `main/display/msg_panel.cpp` 的 `close()`。

## 测试与验证

- 无单元测试框架；**通过烧录后 `idf.py monitor` 看日志验证**
- 涉及外设（GPIO/SPI/I2S/屏幕）改动时，先确认引脚定义与硬件连线一致再烧录
- LVGL 非线程安全：非 LVGL 任务访问 UI 必须 `lvgl_port_lock()`；
  **但 LVGL 上下文内（回调/lv_timer）绝不能再加锁**（锁是非递归的，会自死锁）

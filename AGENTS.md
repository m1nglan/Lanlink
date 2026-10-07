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
| **物理按键** | 录音 **2** ｜ 服务 **42**（均**按下为低、内部上拉**）| `main/main.cpp:40-41` |
| **INMP441 麦克风**（I2S）| BCLK **21** ｜ WS **47** ｜ DIN **1** | `main/drivers/i2s_mic.hpp:29-31` |

⚠️ **n16r8 的 Octal PSRAM 占 GPIO33–37，外设绝不能用这几个脚。**（上表没用到 ✓）

⚠️ **用到了几个"特殊脚"，合法但必须知道：**

| 脚 | 特殊之处 | 用在哪 |
|---|---|---|
| **GPIO45 / GPIO46** | **strapping 脚**（VDD_SPI 电压 / ROM 打印）| 编码器 A、屏 MOSI |
| **GPIO3** | **strapping 脚**（JTAG 源选择）| 屏 RST |
| **GPIO39–42** | **JTAG 默认功能**（MTCK/MTDO/MTDI/MTMS）| 编码器 B=41、服务键=42 |

- 当普通 GPIO 用**没问题**（前提是**不接 JTAG 调试**，且别在启动早期依赖它们的电平）
- **但它们上电瞬间有默认电平** → 将来若再遇到"**开机头几秒引脚状态异常**"（拉低 / 误报按下 / ISR 风暴），**先怀疑这几个**
- 📌 历史上踩过一次"开机 4 秒内 3.3V 引脚被拉低 → 按键误报松开 + ISR 风暴"，**真根因是接地**（不是这些脚），见 [`MD/HANDOFF.md`](MD/HANDOFF.md) §5.0-A

## 构建与运行

用户使用 VS Code 的 ESP-IDF 扩展命令，或终端：
```bash
idf.py build        # 构建
idf.py flash monitor
```

⚠️ **新增源文件后必须 `touch main/CMakeLists.txt`**：`SRC_DIRS` 用 `file(GLOB)` 只在 CMake 配置阶段扫描，
不 touch 则新文件不参与编译，报链接期 `undefined reference`。

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

## 测试与验证

- 无单元测试框架；**通过烧录后 `idf.py monitor` 看日志验证**
- 涉及外设（GPIO/SPI/I2S/屏幕）改动时，先确认引脚定义与硬件连线一致再烧录
- LVGL 非线程安全：非 LVGL 任务访问 UI 必须 `lvgl_port_lock()`；
  **但 LVGL 上下文内（回调/lv_timer）绝不能再加锁**（锁是非递归的，会自死锁）

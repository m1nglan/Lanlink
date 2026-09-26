# Lanlink 项目指南 (ESP32-S3 / ESP-IDF)

> **接手项目请先读 [`HANDOFF.md`](HANDOFF.md)** —— 那里有当前状态、正在排查的问题、源码级验证过的坑。
> 本文件只放长期有效的环境与约定。

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

### 引脚（勿与 PSRAM 冲突）

屏幕 SPI2: SCLK=21 MOSI=20 RST=19 DC=47 CS=48 BL=45 ｜ 编码器: A=7 B=15
物理键: IO10(录音) IO8(服务) ｜ I2S: BCLK=11 WS=12 DIN=13

⚠️ **n16r8 的 Octal PSRAM 占 GPIO33–37，外设绝不能用这几个脚。**

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

## 测试与验证

- 无单元测试框架；**通过烧录后 `idf.py monitor` 看日志验证**
- 涉及外设（GPIO/SPI/I2S/屏幕）改动时，先确认引脚定义与硬件连线一致再烧录
- LVGL 非线程安全：非 LVGL 任务访问 UI 必须 `lvgl_port_lock()`；
  **但 LVGL 上下文内（回调/lv_timer）绝不能再加锁**（锁是非递归的，会自死锁）

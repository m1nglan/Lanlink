# Lanlink 项目指南 (ESP32-S3 / ESP-IDF)

## 项目概述

基于 **ESP-IDF v6.0.1** 的 ESP32-S3 项目（名为 `Lanlink`）。当前处于起步阶段，`main/main.c` 中仅有空的 `app_main()` 入口。与用户交流时请使用**中文**。

## 关键环境

- 目标芯片：`esp32s3`（双核 Xtensa LX7）
- SDK：`D:/esp/v6.0.1/esp-idf`（IDF 版本 **v6.0.1**，API 较旧版本有差异）
- Flash：2MB @ 80MHz；分区表：单应用（`partitions_singleapp.csv`）；**未启用 PSRAM**
- 串口：`COM11`，波特率 115200
- VS Code 使用官方 **ESP-IDF 扩展**，已有专用终端（ESP-IDF Build / Size）

## 构建与运行

在 VS Code 中优先使用 **ESP-IDF 扩展命令**（构建/烧录/监控/擦除）。终端中也可直接运行：

```bash
idf.py build        # 构建（会自动重新运行 CMake）
idf.py flash        # 烧录到 COM11
idf.py monitor      # 串口监控，看 ESP_LOGx 输出
idf.py build flash monitor
```

注意：新增源文件后，CMake 只有在重新配置时才会扫描到新文件——`idf.py build` 会自动完成这一步，无需手动删 `build/`。

## 代码结构与约定

- 应用代码放在 `main/`，通过 `main/CMakeLists.txt` 的 `idf_component_register()` 注册：
  - 新增源文件时更新 `SRCS`（或用 `SRC_DIRS` 自动扫描目录）
  - 新增头文件目录时更新 `INCLUDE_DIRS`
- 应用入口为 `app_main(void)`；通用组件（freertos、log、esp_system 等）默认可用，无需在 `REQUIRES` 中声明
- 使用 `ESP_LOGI/ESP_LOGE` 等日志宏输出，便于 `idf.py monitor` 查看
- 目录下可建立 `driver/` 等子模块存放外设驱动（参考用户既往项目的习惯）

## ESP-IDF 常见坑（务必注意）

1. **FreeRTOS API 需要显式包含头文件**，否则报 `implicit declaration`：
   ```c
   #include "freertos/FreeRTOS.h"
   #include "freertos/task.h"     // vTaskDelay, pdMS_TO_TICKS, xTaskCreate 等
   ```
2. `heap_caps_malloc` 需要 `#include "esp_heap_caps.h"`（或直接用 `malloc` + `esp_heap_caps` 按需分配）
3. **C 与 C++ 混用**：`.cpp` 调用 `.c` 导出的函数时，头文件需用 `extern "C" { ... }` 包裹，否则链接报未定义符号
4. 中断上下文（ISR）中禁用阻塞/延时 API；任务间通信用队列/信号量
5. 定时器/延时单位注意：`pdMS_TO_TICKS(x)` 换算 tick（当前 `CONFIG_FREERTOS_HZ=100`）
6. ESP32-S3 双核：任务默认可跑任意核，如需固定核用任务亲和性参数
7. 修改 `sdkconfig` 相关配置后需重新构建；`sdkconfig` 已被 `.gitignore` 忽略，不入库

## 测试与验证

- 无独立单元测试框架配置；主要通过烧录后 `idf.py monitor` 查看日志验证
- 涉及外设（GPIO/SPI/I2C/摄像头/屏幕）改动时，先确认引脚定义与硬件连线一致再烧录

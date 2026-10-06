# Lanlink 项目问题记录

> ESP32-S3 + ESP-IDF v6.0.1 实时语音转写(讯飞 RTASR)调试问题全记录。

> ## ⚠️ 根因揭示（2026-08-13）
>
> 本文记录的「问题 1（esp_timer 竞态）」「问题 2（PSA 崩溃）」以及 v6.0.2 的"WiFi 链接效应卡死"，**根因都是 `main` 任务栈溢出**（`I2sMic` 的 `m_raw[640]` + `pcm[640]` 超过 3584B 默认栈）。
>
> 「问题 3（射频间歇失灵）」是**真实的硬件问题**（mini 板陶瓷天线），与栈溢出无关。
>
> 详见 **[STACK_OVERFLOW.md](STACK_OVERFLOW.md)**。下文保留作历史排查记录。

## 一、项目概况

- **目标**：INMP441 麦克风 → 16k/16bit 单声道 PCM → 通过 `ws://` 流式发送到讯飞 RTASR → 实时语音转文字
- **主控**：ESP32-S3(双核 Xtensa LX7),IDF **v6.0.1**(`D:/esp/v6.0.1/esp-idf`)
- **开发板**：mini 版,**陶瓷天线(C3 封装)**——已知信号差
- **代码结构**：`main/` 下驱动 `button`、`i2s_mic`、`wifi`、`rtasr`;凭据在 `apikey.h`(gitignore)

## 二、三大问题总结

| # | 问题 | 现象 | 性质 |
|---|------|------|------|
| 1 | WiFi 启动崩溃 | `vTaskGenericNotifyGiveFromISR` 断言 | IDF v6.0.1 **esp_timer 竞态 bug**(间歇) |
| 2 | WPA 握手崩溃 | `xQueueSemaphoreTake` 断言 | IDF v6.0.1 **mbedtls 4.x PSA 线程 bug**(间歇) |
| 3 | 射频间歇失灵 | 扫描 0 AP / 连不上 / `bcn_timeout` | mini 板**陶瓷天线硬件**问题 |

---

## 三、问题 1:WiFi 启动崩溃(esp_timer 竞态)

### 崩溃现场

```
ppTask(WiFi 任务)                    esp_timer 中断(ISR)
  └─ esp_phy_disable                      └─ timer_alarm_isr
      └─ phy_track_pll_deinit                 └─ timer_alarm_handler
          └─ esp_timer_delete                     └─ vTaskGenericNotifyGiveFromISR ← 断言
```

- 位置：`esp_wifi_start()` 阶段 / WiFi 省电 PM 睡眠切换
- 断言：`tasks.c:6213` `vTaskGenericNotifyGiveFromISR`

### 根因

- esp_timer 用**任务通知**唤醒分发任务
- WiFi 的 `esp_phy_disable`(省电/关闭 PHY)会**删除定时器**(`phy_track_pll`)
- 删除与**定时器中断触发**存在竞态 → ISR 通知时目标任务状态冲突 → 断言
- **纯竞态**,间歇性,取决于"删除"与"中断"谁先谁后

### 尝试过的缓解

| 措施 | 效果 |
|------|------|
| WiFi 任务挪到核1(`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_1`) | ⚠️ 只降概率,**又复发** |
| `esp_wifi_set_ps(WIFI_PS_NONE)` 关省电 | 缓解,不根治 |

### 结论

IDF v6.0.1 esp_timer 组件 bug,**只能升级 IDF 根治**。

---

## 四、问题 2:WPA 握手崩溃(PSA 互斥锁)

### 崩溃现场

```
ppTask 处理 EAPOL 握手包
  └─ wpa_pmk_to_ptk → hmac_sha1_vector → psa_import_key
      └─ psa 全局互斥锁 (pthread_mutex_lock)
          └─ xQueueSemaphoreTake ← 断言 pxQueue->uxItemSize == 0
```

- 位置：WPA2 四次握手算 PTK 阶段(`wpa_supplicant`)
- 断言：`queue.c:1713`

### 根因

- WPA 握手用 **mbedtls 4.x 的 PSA 加密库**算 HMAC-SHA1
- PSA 用 **pthread 互斥锁**保护全局密钥槽
- 这些全局互斥锁**只在 `mbedtls_threading_set_alt()` 里初始化**(`threading.c:279`),ESP-IDF 未可靠调用 → 锁未初始化 → `pthread_mutex_lock` 对坏锁调用 → 断言
- 间歇性：初始化与首次使用之间的时序竞态

### 尝试过的缓解

| 措施 | 效果 |
|------|------|
| `WiFi::init()` 开头调 `psa_crypto_init()` | ⚠️ 只初始化 PSA 全局数据,**没初始化那些线程锁**,只降概率,又复发 |

### 可选应急方案

- sdkconfig 关掉 `CONFIG_MBEDTLS_THREADING_PTHREAD` → PSA 不走锁路径,**此崩溃不可能发生**(代价：PSA 无线程保护,对顺序用法影响小)
- 或升级 IDF

---

## 五、问题 3:射频间歇失灵(陶瓷天线)

### 现象

- 扫描 0 个 AP / 连不上 / 连上后 `bcn_timeout`

### 证据链

- 隔离测试(只留 WiFi)能连;完整固件有时也能连 → **固件代码没问题**
- 连上时 RSSI **-15 dBm(超强)**却还丢 beacon → **健康的射频在 -15 绝不可能丢 beacon**
- 换位置/方向,结果就变 → 天线方向性极强、信号在临界点

### 根因

mini 板**陶瓷天线(C3 封装)**增益低、带宽窄、方向性强。**硬件问题,与固件无关。**

### 结论

换天线设计好的板子(**n16r8**:16MB flash + 8MB PSRAM,天线更好)。

---

## 六、为什么"加第四步 RTASR"会触发问题 1、2

- 这两个 bug **本来就潜伏在 IDF v6.0.1 里**,加 RTASR 只是改变了**触发条件**,把它们点着了

### 问题 2(PSA)的触发链

```
RTASR 代码调用 psa_crypto_init()/psa_import_key()
  ↓ PSA 加密实现被激活/链接
  ↓ wpa_supplicant 握手也走 PSA 路径
  ↓ 撞上 PSA 线程互斥锁未初始化 bug → 崩溃
```

### 问题 1(esp_timer)的触发链

```
加了 rtasr:固件 +145KB、SNTP/reader 任务、内存布局与调度时序偏移
  ↓ 时序窗口移到"命中条件"上
  ↓ 触发 esp_timer 潜在竞态 → 间歇崩溃
```

> 本质：业务逻辑没问题,是 IDF v6.0.1 本身不稳,业务代码恰好成了"导火索"。

---

## 七、当前固件状态(截至 2026-08-04)

- 完整固件：`Lanlink.bin` ≈ **0xd9b00**(1MB 分区剩 15%)
- 已实现：WiFi(STA) + 按键 + INMP441 I2S 麦克风 + 讯飞 RTASR(ws:// 不加密) + 流式上传
- 已修复的固件问题：WiFi 省电崩溃(核1)、WPA PSA 崩溃(psa_crypto_init,仅降概率)
- 仍存在：问题 1、2 间歇性复发;问题 3 天线失灵

## 八、结论与建议

1. **软件已写到"能跑通"的程度**,卡在 IDF v6.0.1 版本太新、bug 太多
2. **升级 ESP-IDF 到稳定版**(v6.1+,v6.0.2 大概率不修这两个 bug)→ 修问题 1、2
3. **换 n16r8 板子** → 修问题 3(天线),且 16MB flash 解决分区紧张
4. 业务代码(驱动/CMakeLists/main)升级后基本不用改,补丁/小版本向后兼容

## 九、后续待办

- [ ] 升级 IDF 稳定版后,验证 WiFi 不再崩溃、握手不再崩
- [ ] 在 n16r8 上启用 16MB 分区表 + 8MB PSRAM
- [ ] 验证 RTASR 握手成功(started)与实时识别([中间]/[最终])
- [ ] 如需加密,升级 IDF 后再启用 wss(当前 ws:// 是权衡固件大小的折中)
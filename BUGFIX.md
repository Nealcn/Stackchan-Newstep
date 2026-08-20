# Stackchan Bug 修复记录

> 修复日期：2026-06-12 ~ 2026-06-13  
> 目标设备：M5Stack Core S3 (ESP32-S3)  
> 唤醒词：`"tu dou tu dou"` (土豆土豆)

---

## Bug #1：唤醒词阈值被硬编码覆盖，提示词不生效

### 现象
在 `sdkconfig.defaults` 中配置了自定义唤醒词 `"tu dou tu dou"` 和阈值 `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20`（即 0.2），但实际使用时唤醒词完全不触发。

### 根因
`main/audio/wake_words/custom_wake_word.cc` 第 118 行，在正确读取了配置阈值后，**无条件将其覆盖为硬编码值 `0.5`**：

```cpp
// 第 93-95 行：正确读取配置
#ifdef CONFIG_CUSTOM_WAKE_WORD
    threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;  // 20/100 = 0.2 ✓
    commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, ...});
#endif

// 第 118 行：BUG — 无条件覆盖为 0.5
threshold_ = 0.5f;  // ← 配置值被丢弃
multinet_->set_det_threshold(multinet_model_data_, threshold_);
```

阈值从 0.2 被改成了 0.5，灵敏度大幅降低，导致"土豆土豆"几乎无法被识别。

### 修复
删除 `threshold_ = 0.5f;` 这一行，保留从配置读取的阈值。

**修改文件**: `main/audio/wake_words/custom_wake_word.cc`  
**改动**: 删除第 118 行的 `threshold_ = 0.5f;`

---

## Bug #2：休眠后触摸屏幕无法唤醒

### 现象
设备在 Idle 状态 60 秒后自动进入屏保模式（低亮度），此时触摸屏幕无反应，设备无法唤醒。

### 根因
这是 M5Stack Core S3 特有的**死锁问题**：

**进入休眠** (`OnEnterSleepMode`, `m5stack_core_s3.cc` 第 1769 行)：
```cpp
if (touchpad_timer_) {
    esp_timer_stop(touchpad_timer_);  // 停止触摸轮询定时器
}
```

**唤醒途径** (`PollTouchpad`, 第 1964 行)：
```cpp
if (touch_point.num > 0 && power_save_timer_) {
    power_save_timer_->WakeUp();  // 需要触摸事件才能唤醒
}
```

`PollTouchpad()` 由 `touchpad_timer_` 定时调用。休眠时停止了定时器 → `PollTouchpad` 不再执行 → 触摸事件无法被检测 → `WakeUp()` 永远不会被调用 → **死锁**。

> M5Stack Core S3 的 `PowerSaveTimer` 初始化参数为 `PowerSaveTimer(-1, 60, -1)`，`cpu_max_freq = -1` 表示不会真正进入 ESP light sleep 模式，I2C 总线仍可用，保留触摸轮询是安全的。

### 修复
移除 `OnEnterSleepMode` 中停止 `touchpad_timer_` 的代码，同时移除 `OnExitSleepMode` 中重启定时器的代码。

**修改文件**: `main/boards/m5stack-core-s3/m5stack_core_s3.cc`  
**改动**: 
- 删除 `OnEnterSleepMode` 中的 `esp_timer_stop(touchpad_timer_)`
- 将 `OnExitSleepMode` 中的 `esp_timer_start_periodic(touchpad_timer_, ...)` 替换为空操作

### 屏保相关参数

| 参数 | 值 | 说明 |
|------|-----|------|
| **屏保进入时间** | 60 秒 | `PowerSaveTimer(-1, 60, -1)` - Idle 状态下 60 秒无操作进入 |
| **屏保亮度** | 10 | `GetBacklight()->SetBrightness(10)` |
| **触摸轮询频率** | 20ms (50Hz) | `esp_timer_start_periodic(touchpad_timer_, 20 * 1000)` |
| **是否进入 light sleep** | 否 | `cpu_max_freq_ = -1` 表示不进入硬件休眠 |

---

## Bug #3：sdkconfig.defaults 中无效配置行

### 现象
`sdkconfig.defaults` 第 51 行有一个孤立的字符串 `tu dou`，不在任何 CONFIG 项内。

### 修复
已确认该行不存在于当前文件中。之前由用户选中该行但因不属于有效 Kconfig 配置而不起任何作用。

---

---

## Bug #4：休眠后触摸唤醒经常重启

### 现象
设备进入休眠（屏保）后，点击屏幕唤醒时常发生重启。

### 根因
`OnEnterSleepMode` 中通过 `vTaskSuspend` 强制暂停了 `motion_task_`（BMI270）和 `si12t_task_`（SI12T）。这两个任务与 `PollTouchpad`（FT6336 触摸轮询）**共用同一 I2C 总线**。

`vTaskSuspend` 是无条件强制暂停——如果被暂停的任务正卡在 `i2c_master_transmit_receive()` 内部（持有 I2C 总线互斥锁），锁就永远解不开了。之后 `PollTouchpad` 访问 FT6336 时 I2C 超时 100ms → 触摸被忽略 → 设备无法唤醒。最坏情况：I2C 驱动内部状态不一致导致 panic → 重启。

### 修复
移除 `OnEnterSleepMode` 中的 `vTaskSuspend(motion_task_)` / `vTaskSuspend(si12t_task_)` 以及 `OnExitSleepMode` 中对应的 `vTaskResume`。SM5Stack Core S3 未进入真正的 light sleep（`cpu_max_freq = -1`），I2C 总线在休眠期间仍然可用，ESP-IDF v5 的 I2C 驱动自带总线互斥锁，多任务并发访问是安全的。

**修改文件**: `main/boards/m5stack-core-s3/m5stack_core_s3.cc`

## 修复文件清单

| 文件 | 修改内容 |
|------|----------|
| `main/audio/wake_words/custom_wake_word.cc` | 删除 `threshold_ = 0.5f;` 硬编码 |
| `main/boards/m5stack-core-s3/m5stack_core_s3.cc` | 休眠时保留触摸轮询定时器 |
| `main/boards/m5stack-core-s3/m5stack_core_s3.cc` | 移除 `vTaskSuspend`/`vTaskResume` 避免 I2C 死锁 |
| `main/boards/m5stack-core-s3/m5stack_core_s3.cc` | 唤醒后触摸抑制期 + 完整触摸状态重置 |
| `main/boards/m5stack-core-s3/m5stack_core_s3.cc` | SI12T 摸头加本地反馈 `OnPetted()` |
| `managed_components/78__esp-wifi-connect/wifi_station.cc` | `ESP_ERROR_CHECK` → 安全日志 |
| `main/application.cc` | `HandleNetworkDisconnectedEvent` 加状态回退 |
| `main/application.cc` | `ContinueWakeWordInvoke`/`ContinueOpenAudioChannel` 加 `protocol_` 判空 |
| `main/application.cc` | `OpenAudioChannel` 失败时回退 Idle |
| `main/application.cc` | `WakeWordInvoke` Listening/Speaking 改为注入文本 |
| `main/audio/audio_service.cc` | `PopWakeWordPacket`/`GetLastWakeWord` 加 `wake_word_` 判空 |
| `main/settings.cc` | `nvs_commit` 改为非 abort |
| `sdkconfig.defaults` | 启用多行文字、深色模式、棕出阈值降低、panic 延迟 |

---

## Bug #5：连接中（"连接中..."）之后概率性重启

### 现象
唤醒词触发后显示"连接中..."，此时有一定概率设备重启。

### 根因
三个问题叠加：

**① `ESP_ERROR_CHECK(esp_wifi_set_ps())` abort** — 唤醒时需要切到性能模式 (`WIFI_PS_NONE`)，但如果 WiFi 刚好在瞬态断开/重连，`esp_wifi_set_ps()` 返回非 `ESP_OK`，`ESP_ERROR_CHECK` 直接 `abort()` → 重启。

**② 状态泄漏** — `HandleNetworkDisconnectedEvent` 在断网时关掉了音频通道，但没把状态退回 `Idle`，设备永远卡在"连接中..."。

**③ `protocol_` 空指针** — `ResetProtocol()` 和 `ContinueWakeWordInvoke` 都是通过 `Schedule` 异步执行，如果 `ResetProtocol` 先跑完（`protocol_.reset()`），后续 `ContinueWakeWordInvoke` 解引用空指针 → panic → 重启。

### 修复
1. `wifi_station.cc`：`esp_wifi_set_ps()` 返回值改为 `ESP_LOGW` 日志警告，不再 `abort`
2. `application.cc`：`HandleNetworkDisconnectedEvent` 断网时调用 `SetDeviceState(kDeviceStateIdle)`
3. `application.cc`：`ContinueWakeWordInvoke` 和 `ContinueOpenAudioChannel` 在访问 `protocol_` 前判空

---

## Bug #6：对话中摸头无反馈（"好舒服啊"不触发）

### 现象
对话中（Listening/Speaking）摸头没有反应，设备不再说"好舒服啊"之类的话。

### 根因
上游 `SendUserText` 在 Listening/Speaking 状态下会调用 `SendWakeWordDetected(text)` 将文本注入当前对话，LLM 收到后即可回应。我们的 `SendUserText` 将所有状态路由到 `WakeWordInvoke`，后者在 Listening/Speaking 状态下执行 `AbortSpeaking` / `CloseAudioChannel`，直接关闭了对话，文本丢失。

### 修复
`WakeWordInvoke` 的 Listening/Speaking 分支改为注入文本到当前对话，而不是关闭对话。

**修改文件**: `main/application.cc`
**改动**: 合并 Listening/Speaking 分支，调用 `protocol_->SendWakeWordDetected(wake_word)`

---

## 编译 & 烧录

```bash
# 编译
idf.py build

# 烧录（COM6 为串口号）
idf.py -p COM6 flash
```

> Windows 下用 `build.bat`（项目根目录已提供），设置了 MSYSTEM= 绕过 MSys/MinGW 检测。`idf.py flash` 通过 RTS/DTR 自动控制芯片进入下载模式，无需手动按键。

## 回滚

```bash
# 查看修改
git diff HEAD -- managed_components/78__esp-wifi-connect/wifi_station.cc
git diff HEAD -- main/application.cc

# 单文件回滚
git checkout -- managed_components/78__esp-wifi-connect/wifi_station.cc
git checkout -- main/application.cc
```

---

## 崩溃重启排查参考（乐鑫官方文档，2026-08-20 已验证）

设备经常崩溃重启？优先看这份官方崩溃排查文档，崩溃地址/现象基本都能对上号：

### 核心：严重错误（Fatal Errors）—— Guru Meditation Error / panic / 看门狗 / 栈溢出 / 内存错误 / 掉电复位

- 中文（stable，ESP32-S3）：https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32s3/api-guides/fatal-errors.html
- 英文（stable，ESP32-S3）：https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/fatal-errors.html
- 其他芯片（C3/S3/C6）：把 URL 里的 `esp32s3` 替换成对应型号

### 配套工具

| 场景 | 文档 | 说明 |
|---|---|---|
| 崩溃堆栈自动解码 | [IDF Monitor](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32s3/api-guides/tools/idf-monitor.html) | `idf.py monitor` 把十六进制地址直接翻译成源码文件名+行号，不用手动工具 |
| 偶现/难复现崩溃 | [Core Dump](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32s3/api-guides/core_dump.html) | 崩溃时把内存/任务快照存 Flash，电脑上离线解析 |
| 看门狗（任务/中断/RTC） | [Watchdog (WDTS)](https://docs.espressif.com/projects/esp-idf/zh_CN/stable/esp32s3/api-reference/system/wdts.html) | ⚠️ 正确路径是 `api-reference/system/wdts`（旧的 `watchdog` 路径已 404） |

### 芯片勘误表（硬件 bug 导致的异常重启）

- ESP32 勘误表 PDF：https://www.espressif.com/sites/default/files/documentation/esp32_errata_en.pdf
  - ⚠️ 该站点对部分网络（如国内云服务器）返回 403，浏览器直接访问正常；打不开就用手机/电脑浏览器开

> 排查套路：`idf.py monitor` 看 panic 地址 → fatal-errors 文档对号入座 → 需要保留现场就开 Core Dump → 仍复现不了查勘误表。

// microSD(TF) 卡驱动封装（M5Stack CoreS3，SPI 模式）
//
// ⚠️ GPIO35 复用：LCD-DC（输出）与 SD-MISO（输入）共用同一引脚。
// 所有 SD 操作期间必须将 GPIO35 切为输入（SD 读），完成后切回输出（LCD 驱动），
// 由 SdGpioGuard（RAII）保证任何路径都恢复；同时通过 lvgl 钩子暂停 LCD 刷新，
// 避免 DC 浮空窗口内 LCD 事务把面板刷花（espressif BSP 已知限制：SD 与 LCD 不能同时工作）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_protocol_types.h"   // sdmmc_card_t（ESP-IDF 5.5：sdmmc_card.h 已移除）

namespace stackchan_sd {

// GPIO35 方向守卫：构造→输入(SD-MISO)，析构→输出(LCD-DC)
class SdGpioGuard {
public:
    SdGpioGuard();
    ~SdGpioGuard();
};

class SdCard {
public:
    // 挂载 /sdcard（CS=GPIO4，20MHz 起步，失败自动降速 10M→4M 重试）。
    // 失败不阻塞启动：返回错误码并日志告警，Mounted() 保持 false。
    esp_err_t Init();

    bool Mounted() const { return mounted_; }
    int ActiveFreqKhz() const { return active_freq_khz_; }
    const sdmmc_card_t* card() const { return card_; }

    // LVGL 暂停钩子：SD 操作期间暂停/恢复 LCD 刷新（板级注入 lvgl_port_stop/resume）
    void SetLvglSuspendHook(std::function<void(bool)> hook) { lvgl_hook_ = std::move(hook); }

    // 写文件（自动创建父目录；filename 为空→时间戳命名，未同步时钟回退 photo_NNN.jpg）。
    // 成功返回 true 并填充 out_path（如 /sdcard/photos/20260806_103045.jpg）
    bool WriteFile(const uint8_t* data, size_t len, const std::string& dir,
                   const std::string& filename, std::string* out_path);

    // 读文件：malloc 缓冲区，调用方用 free() 释放
    bool ReadFile(const std::string& path, uint8_t** data, size_t* len);

    bool RemoveFile(const std::string& path);
    int CountFiles(const std::string& dir);  // -1 = 目录不存在

    // 自检：写/读 4KB 随机数据比对 + photos 目录文件数（Init 成功后自动执行）
    void SelfTest();

private:
    bool MountAttempt(int max_freq_khz);
    std::string MakeFilename(const char* ext);

    SemaphoreHandle_t mutex_ = nullptr;
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
    int active_freq_khz_ = 0;
    std::function<void(bool)> lvgl_hook_;  // 可为空（不暂停，降级方案 B）
};

}  // namespace stackchan_sd

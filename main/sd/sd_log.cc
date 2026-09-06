// SD 日志落盘实现（临时诊断功能）
#include "sd_log.h"

#include <cstdarg>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sd_card.h"  // SdGpioGuard：GPIO35(LCD-DC/SD-MISO 复用)方向保护

namespace stackchan_sd {

static FILE* g_log_file = nullptr;
static SemaphoreHandle_t g_log_mutex = nullptr;
static vprintf_like_t g_orig_vprintf = nullptr;
static std::function<void(bool)> g_lvgl_hook;  // 可选：写 SD 期间暂停 LVGL（防花屏）

void SdLog::SetLvglSuspendHook(std::function<void(bool)> hook) {
    g_lvgl_hook = std::move(hook);
}

static int LogVprintfHook(const char* fmt, va_list args) {
    // 1) 原始输出（UART 串口），保持现有日志行为
    int ret = 0;
    if (g_orig_vprintf != nullptr) {
        ret = g_orig_vprintf(fmt, args);
    }

    // 2) 追加写入 TF 卡（仅任务上下文；ISR 内跳过，避免在中断里做文件 I/O）
    if (g_log_file != nullptr && g_log_mutex != nullptr &&
        !portCHECK_IF_IN_ISR()) {
        if (xSemaphoreTake(g_log_mutex, 0) == pdTRUE) {
            {
                // SD 访问必须切 GPIO35 + 暂停 LVGL（与 sd_card WriteFile 同理）
                SdGpioGuard guard;
                if (g_lvgl_hook) g_lvgl_hook(true);
                // va_copy：原 hook 已消费 args，这里用副本格式化
                va_list copy;
                va_copy(copy, args);
                fprintf(g_log_file, "[%10lld] ",
                        (long long)(esp_timer_get_time() / 1000));
                vfprintf(g_log_file, fmt, copy);
                va_end(copy);
                fflush(g_log_file);  // 立即落盘：崩溃/掉电前数据不丢
            }  // guard 析构：GPIO35 恢复输出
            if (g_lvgl_hook) g_lvgl_hook(false);
            xSemaphoreGive(g_log_mutex);
        }
    }
    return ret;
}

bool SdLog::Start(const char* path, size_t max_size) {
    if (g_log_file != nullptr) return true;  // 已开启

    g_log_mutex = xSemaphoreCreateMutex();
    if (g_log_mutex == nullptr) return false;

    // 追加模式打开；若已超过大小上限则截断重写（SD 访问全程 Guard）
    FILE* f = nullptr;
    {
        SdGpioGuard guard;
        if (g_lvgl_hook) g_lvgl_hook(true);
        f = fopen(path, "a");
        if (f != nullptr && fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size > (long)max_size) {
                fclose(f);
                f = fopen(path, "w");
            }
        }
        if (g_lvgl_hook) g_lvgl_hook(false);
    }
    if (f == nullptr) {
        vSemaphoreDelete(g_log_mutex);
        g_log_mutex = nullptr;
        return false;
    }
    g_log_file = f;

    // 挂接日志钩子（保存原输出链）
    g_orig_vprintf = esp_log_set_vprintf(LogVprintfHook);
    ESP_LOGI("SdLog", "TF 卡日志落盘已开启: %s", path);
    return true;
}

void SdLog::Stop() {
    if (g_log_file != nullptr) {
        fclose(g_log_file);
        g_log_file = nullptr;
    }
    if (g_orig_vprintf != nullptr) {
        esp_log_set_vprintf(g_orig_vprintf);
        g_orig_vprintf = nullptr;
    }
    if (g_log_mutex != nullptr) {
        vSemaphoreDelete(g_log_mutex);
        g_log_mutex = nullptr;
    }
}

}  // namespace stackchan_sd

#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // 打印上次重启原因（用于诊断崩溃）
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_PANIC:     ESP_LOGW(TAG, "*** 上次崩溃原因：PANIC (CPU异常) ***"); break;
        case ESP_RST_INT_WDT:   ESP_LOGW(TAG, "*** 上次崩溃原因：中断看门狗(IWDT) ***"); break;
        case ESP_RST_TASK_WDT:  ESP_LOGW(TAG, "*** 上次崩溃原因：任务看门狗(TWDT) ***"); break;
        case ESP_RST_WDT:       ESP_LOGW(TAG, "*** 上次崩溃原因：硬件看门狗 ***"); break;
        case ESP_RST_BROWNOUT:  ESP_LOGW(TAG, "*** 上次崩溃原因：欠压(BROWNOUT) ***"); break;
        case ESP_RST_SW:        ESP_LOGI(TAG, "上次重启：软件重启"); break;
        case ESP_RST_DEEPSLEEP: ESP_LOGI(TAG, "上次重启：深度睡眠唤醒"); break;
        case ESP_RST_POWERON:   ESP_LOGI(TAG, "上次重启：上电复位"); break;
        default:                ESP_LOGI(TAG, "上次重启原因：%d", reason); break;
    }

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    app.Run();  // This function runs the main event loop and never returns
}

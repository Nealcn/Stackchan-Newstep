// microSD(TF) 卡驱动实现（SPI 模式，与 LCD 共用 SPI3 总线）
#include "sd_card.h"

#include <sys/dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "sdmmc/sdmmc_card.h"
#include "sdmmc/sdspi_host.h"

namespace stackchan_sd {

static const char* kTag = "SD";

// CoreS3：GPIO35 = LCD-DC(输出) / SD-MISO(输入) 复用
static constexpr gpio_num_t kDcMisoGpio = GPIO_NUM_35;
static constexpr gpio_num_t kSdCsGpio = GPIO_NUM_4;
static constexpr const char* kMountPoint = "/sdcard";
static constexpr int kSelfTestSize = 4096;

SdGpioGuard::SdGpioGuard() {
    // 抢占为输入：SD MISO 读取
    gpio_set_direction(kDcMisoGpio, GPIO_MODE_INPUT);
}

SdGpioGuard::~SdGpioGuard() {
    // 恢复为输出：LCD DC 驱动
    gpio_set_direction(kDcMisoGpio, GPIO_MODE_OUTPUT);
}

esp_err_t SdCard::Init() {
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
    }
    if (mounted_) return ESP_OK;

    esp_err_t err = ESP_FAIL;
    {
        // 挂载窗口：GPIO35 切输入 + LVGL 暂停；作用域结束后先恢复 GPIO35 再恢复 LVGL
        SdGpioGuard guard;
        if (lvgl_hook_) lvgl_hook_(true);

        const int kFreqAttempts[] = {20000, 10000, 4000};
        for (int freq : kFreqAttempts) {
            if (MountAttempt(freq)) {
                mounted_ = true;
                active_freq_khz_ = freq;
                err = ESP_OK;
                break;
            }
        }
    }  // guard 析构：GPIO35 → OUTPUT
    if (lvgl_hook_) lvgl_hook_(false);  // 再恢复 LVGL

    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD 卡初始化失败：拍照存卡功能不可用（请确认 FAT32 格式且接触良好）");
        return ESP_FAIL;
    }

    ESP_LOGI(kTag, "mounted at %s, freq=%dkHz", kMountPoint, active_freq_khz_);
    sdmmc_card_print_info(stdout, card_);
    SelfTest();
    return ESP_OK;
}

bool SdCard::MountAttempt(int max_freq_khz) {
    // SDSPI_DEFAULT_HOST 默认 slot=SPI2_HOST，必须覆盖为 SPI3_HOST（与 LCD 同总线）
    sdmmc_host_t host = SDSPI_DEFAULT_HOST;
    host.slot = SPI3_HOST;
    host.max_freq_khz = max_freq_khz;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = SPI3_HOST;
    slot_config.gpio_cs = kSdCsGpio;

    esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // 绝不自动格式化（防数据丢失）
        .max_files = 5,
        .allocation_unit_size = 16384,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config,
                                            &mount_config, &card_);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "mount attempt @%dkHz failed: %d", max_freq_khz, err);
        card_ = nullptr;
        return false;
    }
    return true;
}

std::string SdCard::MakeFilename(const char* ext) {
    // 优先时间戳命名；系统时间未同步（<2020）时回退 photo_NNN 计数器
    time_t now = time(nullptr);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 >= 2020) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d%s",
                 tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec, ext);
        return buf;
    }
    static uint32_t counter = 0;
    while (true) {
        char buf[32];
        snprintf(buf, sizeof(buf), "photo_%03u%s", counter++, ext);
        std::string path = std::string(kMountPoint) + "/" + buf;
        if (access(path.c_str(), F_OK) != 0) return buf;  // 不冲突才用
    }
}

bool SdCard::WriteFile(const uint8_t* data, size_t len, const std::string& dir,
                       const std::string& filename, std::string* out_path) {
    if (!mounted_ || data == nullptr) return false;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

    bool ok = false;
    std::string name = filename;
    if (name.empty()) {
        name = MakeFilename(".jpg");
    }
    std::string dir_path = std::string(kMountPoint) + "/" + dir;
    std::string full = dir_path + "/" + name;

    {
        SdGpioGuard guard;
        if (lvgl_hook_) lvgl_hook_(true);

        do {
            if (mkdir(dir_path.c_str(), 0755) != 0 && errno != EEXIST) {
                ESP_LOGE(kTag, "mkdir %s failed: %d", dir_path.c_str(), errno);
                break;
            }
            FILE* f = fopen(full.c_str(), "wb");
            if (f == nullptr) {
                ESP_LOGE(kTag, "fopen %s failed", full.c_str());
                break;
            }
            size_t written = fwrite(data, 1, len, f);
            fclose(f);
            if (written != len) {
                ESP_LOGE(kTag, "fwrite short: %d/%d", (int)written, (int)len);
                break;
            }
            if (out_path != nullptr) *out_path = full;
            ok = true;
            ESP_LOGI(kTag, "written %s (%d bytes)", full.c_str(), (int)len);
        } while (false);
    }  // guard 析构：GPIO35 → OUTPUT
    if (lvgl_hook_) lvgl_hook_(false);  // 再恢复 LVGL

    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    return ok;
}

bool SdCard::ReadFile(const std::string& path, uint8_t** data, size_t* len) {
    if (!mounted_ || data == nullptr || len == nullptr) return false;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

    bool ok = false;
    {
        SdGpioGuard guard;
        if (lvgl_hook_) lvgl_hook_(true);

        do {
            FILE* f = fopen(path.c_str(), "rb");
            if (f == nullptr) break;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0) {
                fclose(f);
                break;
            }
            uint8_t* buf = (uint8_t*)malloc(sz);
            if (buf == nullptr) {
                fclose(f);
                break;
            }
            size_t got = fread(buf, 1, sz, f);
            fclose(f);
            if (got != (size_t)sz) {
                free(buf);
                break;
            }
            *data = buf;
            *len = (size_t)sz;
            ok = true;
        } while (false);
    }  // guard 析构：GPIO35 → OUTPUT
    if (lvgl_hook_) lvgl_hook_(false);

    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    return ok;
}

bool SdCard::RemoveFile(const std::string& path) {
    if (!mounted_) return false;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

    int rc = -1;
    {
        SdGpioGuard guard;
        if (lvgl_hook_) lvgl_hook_(true);
        rc = remove(path.c_str());
    }
    if (lvgl_hook_) lvgl_hook_(false);
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    return rc == 0;
}

int SdCard::CountFiles(const std::string& dir) {
    if (!mounted_) return -1;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);

    int count = -1;
    {
        SdGpioGuard guard;
        if (lvgl_hook_) lvgl_hook_(true);

        DIR* d = opendir(dir.c_str());
        if (d != nullptr) {
            count = 0;
            struct dirent* e;
            while ((e = readdir(d)) != nullptr) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                count++;
            }
            closedir(d);
        }
    }
    if (lvgl_hook_) lvgl_hook_(false);
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    return count;
}

void SdCard::SelfTest() {
    uint8_t wbuf[kSelfTestSize];
    for (int i = 0; i < kSelfTestSize; i++) wbuf[i] = (uint8_t)(esp_random() & 0xFF);

    std::string path;
    if (!WriteFile(wbuf, sizeof(wbuf), ".selftest", "", &path)) {
        ESP_LOGW(kTag, "SelfTest: write failed");
        return;
    }
    uint8_t* rbuf = nullptr;
    size_t rlen = 0;
    if (!ReadFile(path, &rbuf, &rlen) || rlen != sizeof(wbuf) ||
        memcmp(wbuf, rbuf, sizeof(wbuf)) != 0) {
        ESP_LOGW(kTag, "SelfTest: read-back mismatch");
        if (rbuf) free(rbuf);
        return;
    }
    if (rbuf) free(rbuf);
    RemoveFile(path);
    ESP_LOGI(kTag, "SelfTest: write %d bytes ok, read back match", kSelfTestSize);
    ESP_LOGI(kTag, "photos dir files: %d", CountFiles("/sdcard/photos"));
}

}  // namespace stackchan_sd

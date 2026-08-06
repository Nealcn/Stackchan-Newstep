// 拍照存卡实现：Capture → JPEG 编码（复用 image_to_jpeg）→ SD 写入
#include "sd_photo.h"

#include <cstdlib>
#include <string>

#include "application.h"
#include "boards/common/esp_video.h"
#include "esp_log.h"
#include "image_to_jpeg.h"
#include "sd_card.h"

static const char* kTag = "SD.Photo";

namespace {

// 文件名安全过滤：拒绝路径分隔符/空格/..，限长 64，无扩展名自动补 .jpg
bool SanitizeFilename(const std::string& in, std::string* out) {
    if (in.empty()) return false;  // 空 = 默认时间戳命名（由 SdCard 处理）
    if (in.size() > 64) return false;
    for (char c : in) {
        if (c == '/' || c == '\\' || c == ' ') return false;
    }
    if (in.find("..") != std::string::npos) return false;
    *out = in;
    if (out->find('.') == std::string::npos) *out += ".jpg";
    return true;
}

}  // namespace

std::string SavePhotoToSd(EspVideo* camera, stackchan_sd::SdCard* sd,
                          const std::string& filename, std::string* err) {
    if (sd == nullptr || !sd->Mounted()) {
        if (err) *err = "SD 卡未挂载";
        return "";
    }
    if (camera == nullptr) {
        if (err) *err = "摄像头不可用";
        return "";
    }
    std::string safe_name;
    if (!filename.empty() && !SanitizeFilename(filename, &safe_name)) {
        if (err) *err = "文件名不合法（不允许 / \\ .. 和空格）";
        return "";
    }

    TaskPriorityReset priority_reset(1);  // 降优先级，避免编码/写卡期间饿死音频

    if (!camera->Capture()) {
        if (err) *err = "拍照失败（摄像头不可用）";
        return "";
    }

    // 复用 take_photo 同款 JPEG 编码（硬件 JPEG 优先，失败回退软件）
    uint8_t* jpeg = nullptr;
    size_t jpeg_len = 0;
    bool ok = image_to_jpeg(const_cast<uint8_t*>(camera->FrameData()),
                            camera->FrameLen(), camera->FrameWidth(),
                            camera->FrameHeight(), camera->FrameFormat(), 80,
                            &jpeg, &jpeg_len);
    if (!ok || jpeg == nullptr) {
        if (err) *err = "JPEG 编码失败";
        return "";
    }

    std::string path;
    ok = sd->WriteFile(jpeg, jpeg_len, "photos", safe_name, &path);
    free(jpeg);  // image_to_jpeg 输出缓冲区必须用 free() 释放
    if (!ok) {
        if (err) *err = "写入 SD 卡失败";
        return "";
    }
    ESP_LOGI(kTag, "photo saved: %s (%d bytes)", path.c_str(), (int)jpeg_len);
    return path;
}

// 拍照存卡辅助：MCP 工具 self.photo.save 的调用目标
#pragma once

#include <string>

class EspVideo;  // main/boards/common/esp_video.h
namespace stackchan_sd {
class SdCard;
}

// 拍照 → JPEG 编码（复用 image_to_jpeg）→ 写 /sdcard/photos/<name>.jpg
// 成功返回完整保存路径（如 /sdcard/photos/20260806_103045.jpg），失败返回空串并填充 err
std::string SavePhotoToSd(EspVideo* camera, stackchan_sd::SdCard* sd,
                          const std::string& filename, std::string* err);

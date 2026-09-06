// 临时诊断功能：把 ESP_LOG 输出同时写入 TF 卡日志文件。
// 用法：SD 挂载成功后调用 SdLog::Start()；问题排查完成后删除本模块。
#pragma once

#include <cstddef>
#include <functional>

namespace stackchan_sd {

class SdLog {
public:
    // 写 SD 期间暂停/恢复 LVGL 刷新（板级注入；SD 与 LCD 共用 GPIO35）
    static void SetLvglSuspendHook(std::function<void(bool)> hook);

    // 开启日志落盘：追加写入 path（默认 /sdcard/stackchan.log），
    // 文件超过 max_size 字节时从头重写（防止无限增长）。
    // SD 不可用/打开失败时静默返回 false，不影响系统运行。
    static bool Start(const char* path = "/sdcard/stackchan.log",
                      size_t max_size = 512 * 1024);

    // 关闭日志落盘（恢复仅串口输出）
    static void Stop();
};

}  // namespace stackchan_sd

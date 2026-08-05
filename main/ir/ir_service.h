// 红外服务：学习/发射编排 + 摄像头暂停挂钩（RS-1）+ 状态监听
// 线程安全：所有公开方法内部互斥；MCP 工具（主线程）与遥控屏（LVGL 任务）可并发调用
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "ir_driver.h"
#include "ir_store.h"

namespace stackchan_ir {

// 服务状态（供遥控屏/表情/LED 监听）
enum class IrState {
    kIdle,       // 空闲
    kEmitting,   // 发射中（短瞬）
    kLearning,   // 学习中（等待按键）
    kCaptured,   // 已捕获，等待命名保存
    kError,      // 出错（detail 含原因）
};

class IrService {
public:
    using CameraPauseHook = std::function<void(bool paused)>;
    using StateListener = std::function<void(IrState state, const std::string& detail)>;

    // 组装依赖（Init 前设置；全部可空，空则对应能力禁用）
    void Attach(IrDriver* driver, IrStore* store);

    void SetCameraPauseHook(CameraPauseHook hook) { camera_hook_ = std::move(hook); }
    void SetStateListener(StateListener listener) { listener_ = std::move(listener); }

    IrState state() const;
    const std::string& state_detail() const { return detail_; }

    // ---- 发射（阻塞至完成，<200ms）----
    // 成功返回 true；失败时 *err 给出原因（设备不存在/按键未学码/驱动错误）
    bool Emit(const std::string& device_id, const std::string& key, std::string* err);

    // ---- 学习（单次捕获：开始 → 轮询 → 返回码）----
    bool LearnStart(int timeout_ms);   // 进入学习并武装接收；返回 false 表示不可用/已在学
    // 返回值：kCaptured → *out 有效（含协议解析结果）；kLearning → 继续等待；
    //         kIdle（超时/取消）→ 未捕获到
    IrState LearnPoll(IrCode* out);
    void LearnCancel();

    // 学习结果入库（在 kCaptured 状态下调用；设备不存在时自动创建）
    bool SaveLearnedKey(const std::string& device_id, const std::string& device_type,
                        const std::string& device_name, const std::string& key,
                        const IrCode& code);

    // 删除设备（整套按键）或单个按键（支持设备名解析）
    bool RemoveDevice(const std::string& device);
    bool RemoveKey(const std::string& device, const std::string& key);

    // 设备/码库查询（供 MCP 工具描述与遥控屏渲染）
    std::vector<DeviceInfo> ListDevices() const;
    bool GetKey(const std::string& device_id, const std::string& key, IrCode* out) const;
    // 生成不冲突的设备 id（转发 IrStore，屏内/语音学习共用）
    std::string GenerateDeviceId() const;
    // 设备解析:先按 id 精确匹配,再按 name 匹配(语音学习用设备名,老数据 id=name)
    // 找不到时返回原名(保存路径会自动创建)
    std::string ResolveDeviceId(const std::string& name) const;
    // 最近一次捕获的码（kCaptured 状态有效；供语音学习保存/回复）
    IrCode LearnCapturedCode() const;

    // DAT-2：设备云台方位预设（语音联动「先转向家电方位再发码」）
    bool SetDevicePan(const std::string& device_id, int yaw, int pitch);
    bool GetDevicePan(const std::string& device_id, int* yaw, int* pitch) const;

private:
    void SetState(IrState state, const std::string& detail);
    bool LearnTimeoutReached() const;

    mutable SemaphoreHandle_t mutex_ = nullptr;
    IrDriver* driver_ = nullptr;
    IrStore* store_ = nullptr;
    CameraPauseHook camera_hook_;
    StateListener listener_;

    IrState state_ = IrState::kIdle;
    std::string detail_;
    int64_t learn_deadline_us_ = 0;
    IrCode captured_code_;  // LearnPoll 捕获成功时记录（kCaptured 状态有效）

    // 学习轮询定时器：LearnStart 启动（100ms 周期），状态离开 kLearning/kCaptured 时停止。
    // 保证语音学习（无 LVGL 定时器驱动）也能推进捕获/超时；与屏内轮询互斥安全。
    esp_timer_handle_t learn_timer_ = nullptr;
    static void LearnPollTimerCb(void* arg);
};

}  // namespace stackchan_ir

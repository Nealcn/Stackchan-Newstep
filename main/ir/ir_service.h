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

    // 设备/码库查询（供 MCP 工具描述与遥控屏渲染）
    std::vector<DeviceInfo> ListDevices() const;
    bool GetKey(const std::string& device_id, const std::string& key, IrCode* out) const;

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
};

}  // namespace stackchan_ir

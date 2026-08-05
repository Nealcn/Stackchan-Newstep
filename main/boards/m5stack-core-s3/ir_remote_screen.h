// 红外遥控屏（LVGL）：设备列表 + 按键矩阵 + 学习向导
// - 触摸入口：板级 FT6336 注册为 LVGL 输入设备（本文件首个 LVGL indev）
// - 遥控屏打开时板级 PollTouchpad 手势逻辑让渡（RS-3），三区摸头(SI12T)不受影响
// - 学习流程完全离线（DP-7），无需云端
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <lvgl.h>

#include "display/display.h"
#include "ir/ir_service.h"

// 按钮回调载荷（内容区按钮随视图重建，见 content_btns_ 管理）
struct BtnData;

class IrRemoteScreen {
public:
    // LVGL 9 输入设备读取回调（由板级提供，读 FT6336 触摸点）
    using TouchReadFn = std::function<void(lv_indev_t* indev, lv_indev_data_t* data)>;
    // 打开/关闭通知（板级用于让渡触摸手势语义）
    using VisibilityFn = std::function<void(bool open)>;

    // display: 用于 UI 线程互斥（DisplayLockGuard）；ir: 红外服务
    void Init(Display* display, stackchan_ir::IrService* ir, TouchReadFn touch_read,
              VisibilityFn on_visibility);

    void Open();
    void Close();
    bool IsOpen() const { return open_; }

    // 数据变化后刷新（学习完成、码库导入等）
    void Refresh();

private:
    // 锁拆分：*Internal 版本仅限 LVGL 任务上下文调用（事件回调/定时器，勿取锁）；
    // 对外 Open/Close/Refresh 从任意任务调用，内部取 DisplayLockGuard。
    void OpenInternal();
    void CloseInternal();
    void RefreshInternal();

    static void OnButtonClicked(lv_event_t* e);
    static void IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data);

    void BuildUi();
    lv_obj_t* CreateButton(lv_obj_t* parent, lv_coord_t w, lv_coord_t h, const char* text,
                           const char* payload, lv_color_t color);
    void ShowDeviceList();                       // 视图：设备列表
    void ShowKeyMatrix(const std::string& device_id);  // 视图：按键矩阵
    void StartLearn(const std::string& key_name);      // 开始学习指定按键
    void CancelLearn();
    void OnLearnTick();                          // 学习轮询（LVGL 定时器）
    void SetStatus(const char* text);
    void SetStatus(const std::string& text);

    static const char* KeyDisplayName(const std::string& key);

    Display* display_ = nullptr;
    stackchan_ir::IrService* ir_ = nullptr;
    TouchReadFn touch_read_;
    VisibilityFn on_visibility_;
    bool open_ = false;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* back_btn_ = nullptr;
    lv_obj_t* learn_btn_ = nullptr;
    lv_obj_t* content_ = nullptr;    // 视图容器（设备列表 / 按键矩阵）
    lv_obj_t* status_label_ = nullptr;
    lv_timer_t* learn_timer_ = nullptr;
    lv_obj_t* chat_screen_ = nullptr;  // 打开时记录，关闭时恢复
    std::vector<BtnData*> content_btns_;  // 内容区按钮载荷（视图重建时释放）

    std::string current_device_;   // 空 = 设备列表视图
    std::string learning_key_;     // 非空 = 学习中（该按键）
};

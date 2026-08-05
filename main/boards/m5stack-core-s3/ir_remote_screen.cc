// 红外遥控屏实现（LVGL 9）
// 线程模型（重要）：
// - 事件回调 / LVGL 定时器运行在 LVGL 任务内（已持有 lvgl_port 锁）→ 禁止再取 DisplayLockGuard
// - Open/Close/Refresh 可能从板级任务调用 → 必须取 DisplayLockGuard
#include "ir_remote_screen.h"

#include <cstdio>

#include "esp_log.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

const char* kTag = "IR.Screen";

// 按钮回调载荷（动态分配，重建视图时统一释放）
struct BtnData {
    IrRemoteScreen* self;
    std::string payload;  // "back"/"learnbtn"/"newdev"/"open:<id>"/"emit:<key>"/"learn:<key>"
};

// 预设按键（显示名 → 存储键名）；MCP self.ir.send 的 key 使用英文存储名
const struct {
    const char* display;
    const char* key;
} kPresetKeys[] = {
    {"电源", "power"},     {"温度+", "temp_up"}, {"温度-", "temp_down"}, {"模式", "mode"},
    {"风速", "fan_speed"}, {"摆风", "wind_swing"}, {"定时", "timer"},   {"静音", "mute"},
};
constexpr int kPresetKeyCount = sizeof(kPresetKeys) / sizeof(kPresetKeys[0]);

const lv_color_t kBgColor = lv_color_hex(0x101418);
const lv_color_t kBtnColor = lv_color_hex(0x1F2937);
const lv_color_t kAccentColor = lv_color_hex(0x3B82F6);
const lv_color_t kOkColor = lv_color_hex(0x22C55E);
const lv_color_t kTextColor = lv_color_hex(0xE5E7EB);
const lv_color_t kStatusColor = lv_color_hex(0x9CA3AF);

constexpr lv_coord_t kTopBarH = 48;
constexpr lv_coord_t kContentY = kTopBarH;
constexpr lv_coord_t kContentH = 152;
constexpr lv_coord_t kStatusY = kContentY + kContentH;
constexpr lv_coord_t kStatusH = 40;

}  // namespace

void IrRemoteScreen::Init(Display* display, stackchan_ir::IrService* ir, TouchReadFn touch_read,
                          VisibilityFn on_visibility) {
    display_ = display;
    ir_ = ir;
    touch_read_ = std::move(touch_read);
    on_visibility_ = std::move(on_visibility);
    ESP_LOGI(kTag, "Init (screen objects created on first Open)");
}

// ---- LVGL 输入设备读取回调（LVGL 任务内调用，勿取 DisplayLockGuard）----
// 屏未打开时上报 RELEASED，避免干扰聊天界面
void IrRemoteScreen::IndevReadCb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<IrRemoteScreen*>(lv_indev_get_user_data(indev));
    if (self == nullptr || !self->IsOpen() || !self->touch_read_) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    self->touch_read_(indev, data);
}

void IrRemoteScreen::Open() {
    if (open_ || display_ == nullptr) return;
    DisplayLockGuard lock(display_);  // 可能从板级任务调用
    OpenInternal();
}

void IrRemoteScreen::OpenInternal() {
    if (screen_ == nullptr) {
        BuildUi();
    }
    chat_screen_ = lv_screen_active();
    current_device_.clear();
    learning_key_.clear();
    if (learn_timer_ != nullptr) lv_timer_pause(learn_timer_);
    ShowDeviceList();
    lv_screen_load(screen_);
    open_ = true;
    if (on_visibility_) on_visibility_(true);
    ESP_LOGI(kTag, "Opened");
}

void IrRemoteScreen::Close() {
    if (!open_ || display_ == nullptr) return;
    DisplayLockGuard lock(display_);
    CloseInternal();
}

void IrRemoteScreen::CloseInternal() {
    if (!open_) return;
    if (!learning_key_.empty() && ir_ != nullptr) {
        ir_->LearnCancel();
        learning_key_.clear();
    }
    if (learn_timer_ != nullptr) lv_timer_pause(learn_timer_);
    if (chat_screen_ != nullptr) lv_screen_load(chat_screen_);
    open_ = false;
    if (on_visibility_) on_visibility_(false);
    ESP_LOGI(kTag, "Closed");
}

void IrRemoteScreen::Refresh() {
    if (display_ == nullptr) return;
    DisplayLockGuard lock(display_);
    RefreshInternal();
}

void IrRemoteScreen::RefreshInternal() {
    if (current_device_.empty()) {
        ShowDeviceList();
    } else {
        ShowKeyMatrix(current_device_);
    }
}

// ---- 视图构建 ----

void IrRemoteScreen::BuildUi() {
    screen_ = lv_screen_create();
    lv_obj_set_style_bg_color(screen_, kBgColor, 0);
    lv_obj_set_style_text_font(screen_, &BUILTIN_TEXT_FONT, 0);

    // 顶栏
    lv_obj_t* top = lv_obj_create(screen_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 320, kTopBarH);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x0B0F14), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);

    back_btn_ = lv_button_create(top);
    lv_obj_set_size(back_btn_, 56, kTopBarH - 8);
    lv_obj_set_pos(back_btn_, 4, 4);
    lv_obj_set_style_bg_color(back_btn_, kBtnColor, 0);
    lv_obj_set_style_bg_opa(back_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn_, 6, 0);
    lv_obj_t* back_label = lv_label_create(back_btn_);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);
    lv_obj_set_style_text_color(back_label, kTextColor, 0);

    title_label_ = lv_label_create(top);
    lv_obj_set_size(title_label_, 200, kTopBarH);
    lv_obj_align(title_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(title_label_, kTextColor, 0);
    lv_label_set_text(title_label_, "红外遥控");

    learn_btn_ = lv_button_create(top);
    lv_obj_set_size(learn_btn_, 56, kTopBarH - 8);
    lv_obj_set_pos(learn_btn_, 260, 4);
    lv_obj_set_style_bg_color(learn_btn_, kAccentColor, 0);
    lv_obj_set_style_bg_opa(learn_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(learn_btn_, 6, 0);
    lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏，学习中显示为"取消"
    lv_obj_t* learn_label = lv_label_create(learn_btn_);
    lv_label_set_text(learn_label, "取消");
    lv_obj_center(learn_label);
    lv_obj_set_style_text_color(learn_label, kTextColor, 0);

    // 内容区（滚动容器）
    content_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(content_);
    lv_obj_set_size(content_, 320, kContentH);
    lv_obj_set_pos(content_, 0, kContentY);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, 8, 0);
    lv_obj_set_style_pad_column(content_, 8, 0);
    lv_obj_set_style_pad_all(content_, 8, 0);
    lv_obj_clear_flag(content_, LV_OBJ_FLAG_SCROLL_ELASTIC);

    // 状态栏
    status_label_ = lv_label_create(screen_);
    lv_obj_set_size(status_label_, 320, kStatusH);
    lv_obj_set_pos(status_label_, 0, kStatusY);
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_color(status_label_, kStatusColor, 0);
    lv_label_set_text(status_label_, "");

    // 按钮回调
    lv_obj_add_event_cb(back_btn_, OnButtonClicked, LV_EVENT_CLICKED, new BtnData{this, "back"});
    lv_obj_add_event_cb(learn_btn_, OnButtonClicked, LV_EVENT_CLICKED, new BtnData{this, "learnbtn"});

    // 学习轮询定时器（创建即暂停，学习时恢复）
    learn_timer_ = lv_timer_create(
        [](lv_timer_t* t) { static_cast<IrRemoteScreen*>(lv_timer_get_user_data(t))->OnLearnTick(); },
        100, this);
    lv_timer_pause(learn_timer_);

    // 触摸输入设备（LVGL 9：indev 创建 + 回调注册）
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, IndevReadCb);
    lv_indev_set_user_data(indev, this);
}

// 生成一个按钮并挂载回调（载荷数据登记到 content_btns_，重建视图时统一释放）
lv_obj_t* IrRemoteScreen::CreateButton(lv_obj_t* parent, lv_coord_t w, lv_coord_t h,
                                       const char* text, const char* payload,
                                       lv_color_t color) {
    lv_obj_t* btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, kBgColor, 0);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_set_style_text_color(label, kTextColor, 0);
    content_btns_.push_back(new BtnData{this, payload});
    lv_obj_add_event_cb(btn, OnButtonClicked, LV_EVENT_CLICKED, content_btns_.back());
    return btn;
}

void IrRemoteScreen::ShowDeviceList() {
    // 释放上一视图的按钮载荷，再清空容器
    for (auto* d : content_btns_) delete d;
    content_btns_.clear();
    lv_obj_clean(content_);
    lv_label_set_text(title_label_, "红外遥控");
    lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
    SetStatus("返回聊天：点左上角「返回」");

    auto devices = ir_ != nullptr ? ir_->ListDevices() : std::vector<stackchan_ir::DeviceInfo>{};
    for (const auto& dev : devices) {
        std::string payload = "open:" + dev.id;
        CreateButton(content_, 304, 48, dev.name.c_str(), payload.c_str(), kBtnColor);
    }
    // 新设备入口
    CreateButton(content_, 304, 44, "＋ 学习新设备", "newdev", kAccentColor);
}

void IrRemoteScreen::ShowKeyMatrix(const std::string& device_id) {
    current_device_ = device_id;
    for (auto* d : content_btns_) delete d;
    content_btns_.clear();
    lv_obj_clean(content_);

    std::string title;
    const stackchan_ir::DeviceInfo* dev = nullptr;
    if (ir_ != nullptr && !device_id.empty()) {
        for (const auto& d : ir_->ListDevices()) {
            if (d.id == device_id) {
                dev = &d;
                break;
            }
        }
    }
    if (dev != nullptr) {
        title = dev->name;
    } else if (device_id.empty()) {
        title = "新设备：点按键开始学习";
    } else {
        title = device_id;
    }
    lv_label_set_text(title_label_, title.c_str());
    if (learning_key_.empty()) {
        lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
    }
    SetStatus(learning_key_.empty() ? "绿色按键已学码，点击发射；灰色按键点击即学习" : "学习中，请对准遥控器按按键…");

    // 预设按键矩阵 + 已学自定义按键
    for (int i = 0; i < kPresetKeyCount; i++) {
        const char* key = kPresetKeys[i].key;
        bool learned = dev != nullptr && dev->keys.count(key) != 0;
        std::string payload = learned ? ("emit:" + std::string(key)) : ("learn:" + std::string(key));
        lv_obj_t* btn = CreateButton(content_, 74, 44, kPresetKeys[i].display, payload.c_str(),
                                     learned ? kOkColor : kBtnColor);
        if (learned) {
            lv_obj_set_style_border_color(btn, kOkColor, 0);
        }
    }
    // 额外已学按键（不在预设内的自定义按键）
    if (dev != nullptr) {
        for (const auto& kv : dev->keys) {
            bool is_preset = false;
            for (int i = 0; i < kPresetKeyCount; i++) {
                if (kv.first == kPresetKeys[i].key) {
                    is_preset = true;
                    break;
                }
            }
            if (is_preset) continue;
            std::string payload = "emit:" + kv.first;
            CreateButton(content_, 74, 44, kv.first.c_str(), payload.c_str(), kOkColor);
        }
    }
}

// ---- 学习流程 ----

void IrRemoteScreen::StartLearn(const std::string& key_name) {
    if (ir_ == nullptr || !learning_key_.empty()) return;
    if (!ir_->LearnStart(0)) {  // 0 = 服务默认超时（10s）
        SetStatus("学习启动失败（红外模块不可用）");
        return;
    }
    learning_key_ = key_name;
    lv_obj_clear_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
    SetStatus("学习中：请对准遥控器按「" + std::string(KeyDisplayName(key_name)) + "」…");
    lv_timer_resume(learn_timer_);
}

void IrRemoteScreen::CancelLearn() {
    if (ir_ != nullptr) ir_->LearnCancel();
    learning_key_.clear();
    if (learn_timer_ != nullptr) lv_timer_pause(learn_timer_);
    lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
    SetStatus("学习已取消");
}

void IrRemoteScreen::OnLearnTick() {
    if (ir_ == nullptr) return;
    stackchan_ir::IrCode code;
    auto state = ir_->LearnPoll(&code);
    if (state == stackchan_ir::IrState::kCaptured) {
        lv_timer_pause(learn_timer_);
        // 设备为空 = 新设备：自动创建 dev<N>
        std::string dev_id = current_device_;
        if (dev_id.empty()) {
            char buf[16];
            snprintf(buf, sizeof(buf), "dev%d", (int)ir_->ListDevices().size() + 1);
            dev_id = buf;
        }
        std::string key = learning_key_;
        learning_key_.clear();
        bool ok = ir_->SaveLearnedKey(dev_id, "其他", dev_id, key, code);
        SetStatus(ok ? ("已保存 " + key) : "保存失败");
        lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
        RefreshInternal();  // LVGL 任务上下文：刷新按键矩阵（新按键转绿）
    } else if (state == stackchan_ir::IrState::kIdle) {
        lv_timer_pause(learn_timer_);
        learning_key_.clear();
        lv_obj_add_flag(learn_btn_, LV_OBJ_FLAG_HIDDEN);
        SetStatus("学习超时，请重试");
    }
    // kLearning：继续等待
}

// ---- 事件分发 ----

void IrRemoteScreen::OnButtonClicked(lv_event_t* e) {
    auto* data = static_cast<BtnData*>(lv_event_get_user_data(e));
    if (data == nullptr || data->self == nullptr) return;
    IrRemoteScreen* self = data->self;
    const std::string& p = data->payload;

    if (p == "back") {
        if (self->current_device_.empty()) {
            self->CloseInternal();  // LVGL 任务上下文：走无锁内部实现
        } else {
            if (!self->learning_key_.empty()) self->CancelLearn();
            self->current_device_.clear();
            self->ShowDeviceList();
        }
    } else if (p == "learnbtn") {
        self->CancelLearn();
    } else if (p == "newdev") {
        self->ShowKeyMatrix("");
    } else if (p.rfind("open:", 0) == 0) {
        self->ShowKeyMatrix(p.substr(5));
    } else if (p.rfind("emit:", 0) == 0) {
        if (self->ir_ != nullptr) {
            std::string err;
            bool ok = self->ir_->Emit(self->current_device_, p.substr(5), &err);
            self->SetStatus(ok ? "已发送" : err);
        }
    } else if (p.rfind("learn:", 0) == 0) {
        self->StartLearn(p.substr(6));
    }
}

void IrRemoteScreen::SetStatus(const char* text) {
    if (status_label_ != nullptr) lv_label_set_text(status_label_, text);
}

void IrRemoteScreen::SetStatus(const std::string& text) {
    SetStatus(text.c_str());
}

const char* IrRemoteScreen::KeyDisplayName(const std::string& key) {
    for (int i = 0; i < kPresetKeyCount; i++) {
        if (key == kPresetKeys[i].key) return kPresetKeys[i].display;
    }
    return key.c_str();
}

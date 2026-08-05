// 红外服务实现
#include "ir_service.h"

#include "esp_log.h"
#include "ir_codec.h"
#include "ir_config.h"

namespace stackchan_ir {

static const char* kTag = "IR.Service";

void IrService::Attach(IrDriver* driver, IrStore* store) {
    driver_ = driver;
    store_ = store;
    if (mutex_ == nullptr) {
        mutex_ = xSemaphoreCreateMutex();
    }
    detail_ = "ready";
    ESP_LOGI(kTag, "Attached: driver=%s store=%s",
             driver_ != nullptr ? "ok" : "null", store_ != nullptr ? "ok" : "null");
}

IrState IrService::state() const {
    return state_;
}

void IrService::SetState(IrState state, const std::string& detail) {
    state_ = state;
    detail_ = detail;
    ESP_LOGI(kTag, "state -> %d (%s)", (int)state, detail.c_str());
    if (listener_) {
        listener_(state_, detail_);
    }
}

bool IrService::Emit(const std::string& device_id, const std::string& key, std::string* err) {
    if (mutex_ == nullptr || driver_ == nullptr || store_ == nullptr) {
        if (err) *err = "IR service not ready";
        return false;
    }
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(500)) != pdTRUE) {
        if (err) *err = "IR busy (learning in progress)";
        return false;
    }

    bool ok = false;
    std::string reason;
    do {
        if (state_ == IrState::kLearning) {
            reason = "正在学习，请先完成学习";
            break;
        }
        IrCode code;
        if (!store_->GetKey(device_id, key, &code)) {
            reason = "未找到按键码: " + device_id + "." + key +
                     "（可提示用户先学习遥控器）";
            break;
        }
        auto sig = IrCodecRegistry::Instance().Encode(code);
        if (!sig.has_value()) {
            reason = "协议不支持: " + code.protocol;
            break;
        }
        SetState(IrState::kEmitting, device_id + "." + key);
        // RS-1：发码期间暂停摄像头采集（人脸跟随暂停）
        if (camera_hook_) camera_hook_(true);
        ok = driver_->Transmit(*sig);
        if (camera_hook_) camera_hook_(false);
        if (!ok) {
            reason = "发射失败（驱动错误）";
        }
    } while (false);

    SetState(ok ? IrState::kIdle : IrState::kError, ok ? "emitted" : reason);
    xSemaphoreGive(mutex_);
    if (!ok && err) *err = reason;
    return ok;
}

bool IrService::LearnStart(int timeout_ms) {
    if (mutex_ == nullptr || driver_ == nullptr || store_ == nullptr) return false;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(200)) != pdTRUE) return false;
    if (state_ == IrState::kLearning) {
        xSemaphoreGive(mutex_);
        return false;
    }
    if (!driver_->StartCapture()) {
        xSemaphoreGive(mutex_);
        return false;
    }
    int ms = timeout_ms > 0 ? timeout_ms : STACKCHAN_IR_LEARN_DEFAULT_MS;
    learn_deadline_us_ = esp_timer_get_time() + (int64_t)ms * 1000;
    SetState(IrState::kLearning, "请对准遥控器按按键");
    xSemaphoreGive(mutex_);
    return true;
}

bool IrService::LearnTimeoutReached() const {
    return learn_deadline_us_ != 0 && esp_timer_get_time() >= learn_deadline_us_;
}

IrState IrService::LearnPoll(IrCode* out) {
    if (mutex_ == nullptr || driver_ == nullptr) return IrState::kIdle;
    if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) != pdTRUE) return IrState::kLearning;

    if (state_ != IrState::kLearning) {
        xSemaphoreGive(mutex_);
        return state_;
    }

    IrState result = IrState::kLearning;
    RawSignal sig;
    if (driver_->PollCapture(&sig)) {
        if (sig.Valid()) {
            auto code = IrCodecRegistry::Instance().Decode(sig);
            if (code.has_value() && !code->repeat) {
                if (out != nullptr) *out = *code;
                SetState(IrState::kCaptured, code->protocol + " " + std::to_string(code->bits) + "bit");
                result = IrState::kCaptured;
            } else if (code.has_value() && code->repeat) {
                // 重复帧：继续等待数据帧
                ESP_LOGI(kTag, "repeat frame ignored, keep waiting");
                driver_->StartCapture();  // 重新武装
                learn_deadline_us_ = esp_timer_get_time() + 3000 * 1000;  // 延长 3s
            } else {
                ESP_LOGW(kTag, "captured but undecodable, keep waiting");
                driver_->StartCapture();
            }
        } else {
            driver_->StartCapture();  // 空捕获（噪声），重新武装
        }
    } else if (LearnTimeoutReached()) {
        driver_->CancelCapture();
        SetState(IrState::kIdle, "学习超时");
        result = IrState::kIdle;
    }
    xSemaphoreGive(mutex_);
    return result;
}

void IrService::LearnCancel() {
    if (mutex_ == nullptr) return;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (state_ == IrState::kLearning || state_ == IrState::kCaptured) {
        driver_->CancelCapture();
        SetState(IrState::kIdle, "学习已取消");
    }
    xSemaphoreGive(mutex_);
}

bool IrService::SaveLearnedKey(const std::string& device_id, const std::string& device_type,
                               const std::string& device_name, const std::string& key,
                               const IrCode& code) {
    if (mutex_ == nullptr || store_ == nullptr) return false;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool ok = false;
    do {
        if (state_ != IrState::kCaptured) break;
        if (!store_->FindDevice(device_id)) {
            DeviceInfo dev;
            dev.id = device_id;
            dev.type = device_type.empty() ? "其他" : device_type;
            dev.name = device_name.empty() ? device_id : device_name;
            if (!store_->AddDevice(dev)) break;
        }
        ok = store_->SetKey(device_id, key, code);
    } while (false);
    SetState(ok ? IrState::kIdle : IrState::kError,
             ok ? "已保存 " + device_id + "." + key : "保存失败");
    xSemaphoreGive(mutex_);
    return ok;
}

std::vector<DeviceInfo> IrService::ListDevices() const {
    if (store_ == nullptr) return {};
    return store_->ListDevices();
}

bool IrService::GetKey(const std::string& device_id, const std::string& key, IrCode* out) const {
    if (store_ == nullptr) return false;
    return store_->GetKey(device_id, key, out);
}

}  // namespace stackchan_ir

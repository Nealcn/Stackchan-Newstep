// RMT 红外收发驱动实现（ESP-IDF v5.x）
#include "ir_driver.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "ir_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace stackchan_ir {

static const char* TAG = "IR.Driver";

namespace {

constexpr size_t kRxMemBlockSymbols = 64;   // RMT RX 通道内存（偶数）
constexpr size_t kRxBufferSymbols = 128;    // 捕获缓冲（NEC 全帧约 34 符号，余量充足）
constexpr size_t kTxMemBlockSymbols = 64;   // RMT TX 通道内存（NEC 全帧 33 符号）
constexpr size_t kTxQueueDepth = 4;

// 归一化辅助：相邻同电平段合并
void AppendSegment(std::vector<std::pair<bool, uint32_t>>* seq, bool on, uint32_t us) {
    if (us == 0) return;
    if (!seq->empty() && seq->back().first == on) {
        seq->back().second += us;
    } else {
        seq->emplace_back(on, us);
    }
}

}  // namespace

IrDriver::~IrDriver() {
    Deinit();
}

void IrDriver::Deinit() {
    if (rx_ != nullptr) {
        rmt_disable(rx_);
        rmt_del_channel(rx_);
        rx_ = nullptr;
    }
    if (tx_ != nullptr) {
        rmt_disable(tx_);
        rmt_del_channel(tx_);
        tx_ = nullptr;
    }
    if (copy_enc_ != nullptr) {
        rmt_del_encoder(copy_enc_);
        copy_enc_ = nullptr;
    }
    if (rx_sem_ != nullptr) {
        vSemaphoreDelete(rx_sem_);
        rx_sem_ = nullptr;
    }
    rx_buffer_.clear();
    rx_done_.store(false);
    rx_cancel_.store(false);
}

esp_err_t IrDriver::Init(int tx_gpio, int rx_gpio) {
    if (tx_gpio == GPIO_NUM_NC || rx_gpio == GPIO_NUM_NC) {
        ESP_LOGE(TAG, "Init failed: IR GPIO not configured (TC-8). "
                      "Set STACKCHAN_IR_TX_GPIO / STACKCHAN_IR_RX_GPIO in ir_config.h");
        return ESP_ERR_INVALID_ARG;
    }
    // 幂等：重复调用先释放
    Deinit();

    // ---- TX 通道 ----
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num = static_cast<gpio_num_t>(tx_gpio),
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STACKCHAN_IR_TX_RESOLUTION_HZ,
        .mem_block_symbols = kTxMemBlockSymbols,
        .trans_queue_depth = kTxQueueDepth,
        .intr_priority = 0,
        .flags = {0},
    };
    if (rmt_new_tx_channel(&tx_cfg, &tx_) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed");
        Deinit();
        return ESP_FAIL;
    }
    rmt_copy_encoder_config_t enc_cfg = {};
    if (rmt_new_copy_encoder(&enc_cfg, &copy_enc_) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed");
        Deinit();
        return ESP_FAIL;
    }

    // ---- 载波（38kHz）----
    rmt_carrier_config_t carrier_cfg = {
        .frequency_hz = STACKCHAN_IR_CARRIER_HZ,
        .duty_cycle = STACKCHAN_IR_CARRIER_DUTY,
        .flags = {
            .polarity_active_low = false,  // 默认：符号电平=1 时输出载波
            .always_on = false,
        },
    };
    esp_err_t err = rmt_apply_carrier(tx_, &carrier_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_apply_carrier failed (err=%d): carrier disabled", err);
    }
    if (rmt_enable(tx_) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable(tx) failed");
        Deinit();
        return ESP_FAIL;
    }

    // ---- RX 通道 ----
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = static_cast<gpio_num_t>(rx_gpio),
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STACKCHAN_IR_RX_RESOLUTION_HZ,
        .mem_block_symbols = kRxMemBlockSymbols,
        .intr_priority = 0,
        .flags = {0},
    };
    if (rmt_new_rx_channel(&rx_cfg, &rx_) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_rx_channel failed");
        Deinit();
        return ESP_FAIL;
    }
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = IrDriver::OnRxDone,
    };
    err = rmt_rx_register_event_callbacks(rx_, &cbs, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_rx_register_event_callbacks failed: %d", err);
        Deinit();
        return ESP_FAIL;
    }
    if (rmt_enable(rx_) != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable(rx) failed");
        Deinit();
        return ESP_FAIL;
    }

    rx_buffer_.resize(kRxBufferSymbols);
    rx_sem_ = xSemaphoreCreateBinary();
    if (rx_sem_ == nullptr) {
        ESP_LOGE(TAG, "xSemaphoreCreateBinary failed");
        Deinit();
        return ESP_ERR_NO_MEM;
    }
    rx_done_.store(false);
    rx_cancel_.store(false);

    ESP_LOGI(TAG, "Init OK: TX=GPIO%d RX=GPIO%d carrier=%uHz", tx_gpio, rx_gpio,
             (unsigned)STACKCHAN_IR_CARRIER_HZ);
    return ESP_OK;
}

bool IrDriver::Transmit(const RawSignal& sig) {
    if (tx_ == nullptr || !sig.Valid()) return false;

    std::vector<rmt_symbol_word_t> syms;
    BuildSymbols(sig, &syms);
    if (syms.empty()) return false;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,  // 单次发送
        // 发送结束输出低电平（无载波）;ESP-IDF 5.3+ 移除了 RMT_IDLE_LEVEL 枚举,
        // eot_level 收敛为 flags 位域: 0=低电平, 1=高电平
        .flags = {.eot_level = 0, .queue_nonblocking = false},
    };
    esp_err_t err = rmt_transmit(tx_, copy_enc_, syms.data(), syms.size() * sizeof(rmt_symbol_word_t),
                                 &tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %d", err);
        return false;
    }
    // 阻塞等待硬件播放完成（copy encoder 在 transmit 时同步拷贝，此处确保返回前播放完毕）
    err = rmt_tx_wait_all_done(tx_, pdMS_TO_TICKS(STACKCHAN_IR_TX_WAIT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rmt_tx_wait_all_done timeout: %d", err);
    }
    return true;
}

bool IrDriver::StartCapture() {
    if (rx_ == nullptr) return false;
    rx_done_.store(false);
    rx_cancel_.store(false);
    rmt_receive_config_t recv_cfg = {
        .signal_range_min_ns = STACKCHAN_IR_RX_MIN_NS,
        .signal_range_max_ns = STACKCHAN_IR_RX_MAX_NS,
        .flags = {0},
    };
    esp_err_t err = rmt_receive(rx_, rx_buffer_.data(), rx_buffer_.size() * sizeof(rmt_symbol_word_t),
                                &recv_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_receive failed: %d", err);
        return false;
    }
    return true;
}

bool IrDriver::PollCapture(RawSignal* out) {
    if (rx_sem_ == nullptr || out == nullptr) return false;
    if (xSemaphoreTake(rx_sem_, 0) != pdTRUE) return false;
    if (rx_cancel_.load()) {
        rx_done_.store(false);
        return false;
    }
    uint32_t count = rx_symbol_count_.load();
    if (count == 0 || count > rx_buffer_.size()) {
        rx_done_.store(false);
        return false;
    }
    *out = NormalizeSymbols(rx_buffer_.data(), count);
    rx_done_.store(false);
    return true;
}

void IrDriver::CancelCapture() {
    rx_cancel_.store(true);
}

bool IrDriver::OnRxDone(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* data,
                        void* ctx) {
    auto* self = static_cast<IrDriver*>(ctx);
    (void)ch;
    if (data != nullptr) {
        self->rx_symbol_count_.store(data->num_symbols);
    }
    self->rx_done_.store(true);
    BaseType_t need_yield = pdFALSE;
    if (self->rx_sem_ != nullptr) {
        xSemaphoreGiveFromISR(self->rx_sem_, &need_yield);
    }
    return need_yield == pdTRUE;  // 返回 true 触发调度
}

void IrDriver::BuildSymbols(const RawSignal& sig, std::vector<rmt_symbol_word_t>* syms) const {
    syms->clear();
    const auto& d = sig.durations_us;
    // 交替序列：偶数下标=on，奇数下标=off
    for (size_t i = 0; i + 1 < d.size(); i += 2) {
        if (d[i] == 0) continue;
        rmt_symbol_word_t s = {};
        s.level0 = 1;  // 电平 1 输出载波（carrier polarity_active_low=false）
        s.duration0 = d[i];
        s.level1 = 0;
        s.duration1 = d[i + 1];
        syms->push_back(s);
    }
    // 尾部孤立 on 段（无配对 off）
    if ((d.size() % 2) == 1 && d.back() != 0) {
        rmt_symbol_word_t s = {};
        s.level0 = 1;
        s.duration0 = d.back();
        s.level1 = 0;
        s.duration1 = 0;
        syms->push_back(s);
    }
}

RawSignal IrDriver::NormalizeSymbols(const rmt_symbol_word_t* syms, size_t count) {
    RawSignal out;
    if (syms == nullptr || count == 0) return out;

    // 1) 展开为 (on, us) 段序列（含同电平合并）
    std::vector<std::pair<bool, uint32_t>> seq;
    const bool active_low = STACKCHAN_IR_RX_ACTIVE_LOW;
    for (size_t i = 0; i < count; i++) {
        const auto& s = syms[i];
        if (s.duration0 != 0) {
            bool on = (s.level0 == 0) == active_low;
            AppendSegment(&seq, on, s.duration0);
        }
        if (s.duration1 != 0) {
            bool on = (s.level1 == 0) == active_low;
            AppendSegment(&seq, on, s.duration1);
        }
    }
    if (seq.empty()) return out;

    // 2) 丢弃前导空闲段（应为 off），并归一化为"第 0 段 = on"
    size_t start = 0;
    if (!seq[start].first) start++;
    if (start >= seq.size()) return out;

    // 3) 转换成交替时长序列
    for (size_t i = start; i < seq.size(); i++) {
        out.durations_us.push_back(seq[i].second);
    }
    return out;
}

}  // namespace stackchan_ir

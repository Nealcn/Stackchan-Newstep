// RMT 红外收发驱动：与硬件的唯一交互点
// 基于 ESP-IDF v5.x RMT 驱动（esp_driver_rmt）
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace stackchan_ir {

// 原始红外信号：交替的 on/off 时长序列（微秒），第 0 段为"载波开启"
struct RawSignal {
    std::vector<uint32_t> durations_us;

    bool Valid() const { return !durations_us.empty(); }
};

class IrDriver {
public:
    ~IrDriver();

    // 初始化 TX/RX 通道并启用（幂等：重复调用先 Deinit）
    // tx_gpio / rx_gpio 为 GPIO_NUM_NC 时返回 ESP_ERR_INVALID_ARG
    esp_err_t Init(int tx_gpio, int rx_gpio);

    bool tx_ready() const { return tx_ != nullptr; }
    bool rx_ready() const { return rx_ != nullptr; }

    // ---- 发射 ----
    // 阻塞至硬件播放完成（或超时）。线程安全由调用方（IrService 互斥锁）保证。
    bool Transmit(const RawSignal& sig);

    // ---- 接收（单次捕获，一次一发）----
    // 开始捕获：清除上次标志并重新武装 RMT RX
    bool StartCapture();
    // 非阻塞查询捕获结果；返回 true 表示本次捕获完成（数据已归一化到 out）
    bool PollCapture(RawSignal* out);
    // 停止捕获意图：仅置标志，当前已武装的接收会自然超时完成并被忽略
    void CancelCapture();

private:
    void Deinit();  // 释放全部通道/编码器/信号量并置空（幂等）

    static bool OnRxDone(rmt_channel_handle_t ch,
                         const rmt_rx_done_event_data_t* data, void* ctx);

    // 将 RawSignal 转成 RMT 符号序列（1 符号 = 1 段 on + 1 段 off）
    void BuildSymbols(const RawSignal& sig, std::vector<rmt_symbol_word_t>* syms) const;
    // 将 RMT 符号序列归一化为 RawSignal（处理极性、前导空闲、相邻同段合并）
    static RawSignal NormalizeSymbols(const rmt_symbol_word_t* syms, size_t count);

    rmt_channel_handle_t tx_ = nullptr;
    rmt_channel_handle_t rx_ = nullptr;
    rmt_encoder_handle_t copy_enc_ = nullptr;

    // RX 捕获状态（ISR 与任务之间通过信号量+原子标志通信）
    SemaphoreHandle_t rx_sem_ = nullptr;
    std::atomic<bool> rx_done_{false};
    std::atomic<bool> rx_cancel_{false};
    std::atomic<uint32_t> rx_symbol_count_{0};
    std::vector<rmt_symbol_word_t> rx_buffer_;  // 捕获缓冲（128 符号）
};

}  // namespace stackchan_ir

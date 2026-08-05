// NEC 协议编解码器（含 NEC-ext 16 位地址）
// 时序（38kHz 载波）：
//   AGC:   on 9000us, off 4500us
//   bit 0: on 560us,  off 560us
//   bit 1: on 560us,  off 1690us
//   重复帧: on 9000us, off 2250us（解码时识别为 repeat，不入码库）
#pragma once

#include "ir_codec.h"

namespace stackchan_ir {

class NecCodec : public IrCodec {
public:
    const char* Name() const override { return "NEC"; }

    std::optional<RawSignal> Encode(const IrCode& code) const override;
    std::optional<IrCode> Decode(const RawSignal& sig) const override;

    // 时序常量（微秒）
    static constexpr uint32_t kAgcOnUs = 9000;
    static constexpr uint32_t kAgcOffUs = 4500;
    static constexpr uint32_t kBitOnUs = 560;
    static constexpr uint32_t kZeroOffUs = 560;
    static constexpr uint32_t kOneOffUs = 1690;
    static constexpr uint32_t kRepeatOffUs = 2250;

    // 容差（微秒）：on/off 段在 [标称 - tol, 标称 + tol] 内视为匹配
    static constexpr uint32_t kAgcTolUs = 2500;  // AGC on 允许 ±2500
    static constexpr uint32_t kBitTolUs = 250;   // 位脉冲允许 ±250

    // 位判决阈值：off < kZeroMaxUs 判 0，> kOneMinUs 判 1
    static constexpr uint32_t kZeroMaxUs = 1000;
    static constexpr uint32_t kOneMinUs = 1300;
};

}  // namespace stackchan_ir

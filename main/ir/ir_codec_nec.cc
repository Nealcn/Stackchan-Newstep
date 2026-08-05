// NEC 协议编解码实现
#include "ir_codec_nec.h"

#include <cstdlib>

#include "esp_log.h"

namespace stackchan_ir {

namespace {
const char* kTag = "IR.NEC";

bool Near(uint32_t v, uint32_t target, uint32_t tol) {
    return v >= target - tol && v <= target + tol;
}
}  // namespace

std::optional<RawSignal> NecCodec::Encode(const IrCode& code) const {
    if (code.bits != 32 && code.bits != 16) {
        ESP_LOGW(kTag, "Encode: unsupported bit width %u (NEC: 16/32)", code.bits);
        return std::nullopt;
    }

    RawSignal sig;
    auto& d = sig.durations_us;
    d.reserve(2 + code.bits * 2);

    // AGC 引导
    d.push_back(kAgcOnUs);
    d.push_back(kAgcOffUs);
    // 数据位（LSB-first）
    for (uint16_t i = 0; i < code.bits; i++) {
        d.push_back(kBitOnUs);
        d.push_back(((code.data >> i) & 1ULL) ? kOneOffUs : kZeroOffUs);
    }
    return sig;
}

std::optional<IrCode> NecCodec::Decode(const RawSignal& sig) const {
    const auto& d = sig.durations_us;
    if (d.size() < 2) return std::nullopt;

    // 1) 定位 AGC 段：跳过前导噪声段，再跳过「前导重复帧」（重复帧后还有下一对才算前导）
    size_t i = 0;
    while (i < d.size() && d[i] < kAgcOnUs - kAgcTolUs) i += 2;
    while (i + 2 < d.size() && Near(d[i], kAgcOnUs, kAgcTolUs) &&
           Near(d[i + 1], kRepeatOffUs, kBitTolUs)) {
        i += 2;  // 前导重复帧（长按捕获从重复帧开始）
    }
    if (i >= d.size()) return std::nullopt;
    if (!Near(d[i], kAgcOnUs, kAgcTolUs)) return std::nullopt;

    // 2) AGC off：4500 = 数据帧，2250 = 重复帧（独立重复帧直接返回）
    if (i + 1 >= d.size()) return std::nullopt;
    IrCode code;
    code.protocol = Name();
    if (Near(d[i + 1], kRepeatOffUs, kBitTolUs)) {
        code.repeat = true;
        code.bits = 0;
        return code;
    }
    if (!Near(d[i + 1], kAgcOffUs, kAgcTolUs)) return std::nullopt;

    // 3) 解析数据位（LSB-first）；遇重复帧起始（≈9000/2250）即结束数据位
    i += 2;
    uint64_t data = 0;
    uint16_t bit = 0;
    while (i + 1 < d.size()) {
        if (Near(d[i], kAgcOnUs, kAgcTolUs) && Near(d[i + 1], kRepeatOffUs, kBitTolUs)) {
            break;  // 重复帧起始（长按捕获：数据帧后跟重复帧）
        }
        if (!Near(d[i], kBitOnUs, kBitTolUs)) return std::nullopt;
        uint32_t off = d[i + 1];
        if (off < kZeroMaxUs) {
            // 0
        } else if (off > kOneMinUs) {
            data |= (1ULL << bit);
        } else {
            return std::nullopt;  // 无法判决
        }
        bit++;
        if (bit > 32) return std::nullopt;  // 超出 NEC 上限
        i += 2;
    }

    // 4) 吃掉帧尾重复帧（长按捕获的尾部）
    while (i + 1 < d.size() && Near(d[i], kAgcOnUs, kAgcTolUs) &&
           Near(d[i + 1], kRepeatOffUs, kBitTolUs)) {
        i += 2;
    }

    if (bit != 32 && bit != 16) {
        ESP_LOGW(kTag, "Decode: %u bits (NEC requires 16/32)", bit);
        return std::nullopt;
    }
    code.data = data;
    code.bits = bit;
    code.repeat = false;
    return code;
}

}  // namespace stackchan_ir

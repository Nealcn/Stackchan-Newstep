// 红外协议编解码抽象与注册表
// 码库以「协议 + 码值 + 位数」存储，与驱动时序解耦（ADR-5）
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ir_driver.h"

namespace stackchan_ir {

// 一条家电红外码（协议无关表示）
struct IrCode {
    std::string protocol;  // 如 "NEC"
    uint64_t data = 0;     // 码值（LSB-first 序列化）
    uint16_t bits = 0;     // 有效位数（如 32/16）
    bool repeat = false;   // 是否为重复帧（仅解码输出用）

    bool Valid() const { return !protocol.empty() && bits > 0; }
};

// 协议编解码器接口
class IrCodec {
public:
    virtual ~IrCodec() = default;

    virtual const char* Name() const = 0;
    // 码 → 时序；失败返回 std::nullopt
    virtual std::optional<RawSignal> Encode(const IrCode& code) const = 0;
    // 时序 → 码；失败返回 std::nullopt
    virtual std::optional<IrCode> Decode(const RawSignal& sig) const = 0;
};

// 编解码器注册表（按协议名查找）
class IrCodecRegistry {
public:
    static IrCodecRegistry& Instance();

    void Register(const IrCodec* codec);            // 注册（重复同名覆盖）
    const IrCodec* Find(const std::string& protocol) const;
    std::vector<std::string> ListProtocols() const;

    // 便捷入口：任意已注册协议编解码
    std::optional<RawSignal> Encode(const IrCode& code) const;
    std::optional<IrCode> Decode(const RawSignal& sig) const;

private:
    IrCodecRegistry();
    std::vector<const IrCodec*> codecs_;
};

}  // namespace stackchan_ir

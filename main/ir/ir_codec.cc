// 红外协议编解码注册表实现
#include "ir_codec.h"

#include "ir_codec_nec.h"

namespace stackchan_ir {

IrCodecRegistry::IrCodecRegistry() {
    // 内置协议注册（后续协议在此追加，或由外部调用 Register）
    static const NecCodec kNecCodec;
    Register(&kNecCodec);
}

IrCodecRegistry& IrCodecRegistry::Instance() {
    static IrCodecRegistry registry;
    return registry;
}

void IrCodecRegistry::Register(const IrCodec* codec) {
    if (codec == nullptr) return;
    for (size_t i = 0; i < codecs_.size(); i++) {
        if (std::string(codecs_[i]->Name()) == codec->Name()) {
            codecs_[i] = codec;  // 同名覆盖
            return;
        }
    }
    codecs_.push_back(codec);
}

const IrCodec* IrCodecRegistry::Find(const std::string& protocol) const {
    for (const auto* c : codecs_) {
        if (protocol == c->Name()) return c;
    }
    return nullptr;
}

std::vector<std::string> IrCodecRegistry::ListProtocols() const {
    std::vector<std::string> out;
    for (const auto* c : codecs_) out.emplace_back(c->Name());
    return out;
}

std::optional<RawSignal> IrCodecRegistry::Encode(const IrCode& code) const {
    const IrCodec* c = Find(code.protocol);
    if (c == nullptr) return std::nullopt;
    return c->Encode(code);
}

std::optional<IrCode> IrCodecRegistry::Decode(const RawSignal& sig) const {
    // 按注册顺序尝试解析（协议无关的时序 → 码）
    for (const auto* c : codecs_) {
        auto code = c->Decode(sig);
        if (code.has_value()) return code;
    }
    return std::nullopt;
}

}  // namespace stackchan_ir

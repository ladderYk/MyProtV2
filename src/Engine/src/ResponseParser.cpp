// src/Engine/src/ResponseParser.cpp
// 响应解析器实现 — 单标签响应解析 (C++11, ADR-0010 §5)

#include "MyProt/Engine/ResponseParser.hpp"
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace MyProt { namespace Engine {

namespace {

int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// 校验 validCondition 子集 "resp[N]==V"
bool CheckValidCondition(const Core::ByteView& resp, const std::string& cond) {
    size_t lb = cond.find("resp[");
    if (lb == std::string::npos) return false;
    size_t rb = cond.find(']', lb);
    size_t eq = cond.find("==", rb == std::string::npos ? lb : rb);
    if (rb == std::string::npos || eq == std::string::npos) return false;

    int idx = std::atoi(cond.substr(lb + 5, rb - lb - 5).c_str());
    if (idx < 0 || static_cast<size_t>(idx) >= resp.size) return false;

    // 跳过 "==" 后的空白
    size_t vpos = eq + 2;
    while (vpos < cond.size() && (cond[vpos] == ' ' || cond[vpos] == '\t')) ++vpos;
    uint32_t expect = static_cast<uint32_t>(std::strtoul(cond.c_str() + vpos, 0, 0));
    return resp[static_cast<size_t>(idx)] == expect;
}

/// 依 finalType 填充 TypedValue; 成功 true / 类型不支持或数据不足 false
/// Bool 两种读法: 声明了 BitOffset(>=0) → 位提取 (raw[0]>>bit)&1;
///        未声明(-1) → raw[0]!=0 (整寄存器真假判断, 兼容按寄存器读的标签).
bool FillTypedValue(const uint8_t* raw, size_t rawLen,
                    const std::string& finalType, Core::ByteOrder bo,
                    int bitOffset,
                    Core::TypedValue& out) {
    if (finalType == "ByteArray") {
        out.type = Core::ValueType::ByteArray;
        out.bytes.assign(raw, raw + rawLen);
        return true;
    }
    if (finalType == "String") {
        out.type = Core::ValueType::String;
        out.str.assign(reinterpret_cast<const char*>(raw), rawLen);
        return true;
    }
    if (finalType == "Bool") {
        if (rawLen < 1) return false;
        out.type = Core::ValueType::Bool;
        if (bitOffset >= 0) {
            out.b = ((raw[0] >> bitOffset) & 1) != 0;   // 位提取
        } else {
            out.b = raw[0] != 0;                        // 整寄存器真假
        }
        return true;
    }

    struct TypeSpec { const char* name; Core::ValueType vt; size_t width; bool isSigned; };
    static const TypeSpec specs[] = {
        { "UInt16", Core::ValueType::UInt16, 2, false },
        { "Int16",  Core::ValueType::Int16,  2, true  },
        { "UInt32", Core::ValueType::UInt32, 4, false },
        { "Int32",  Core::ValueType::Int32,  4, true  },
        { "UInt64", Core::ValueType::UInt64, 8, false },
        { "Int64",  Core::ValueType::Int64,  8, true  },
        { "Float",  Core::ValueType::Float,  4, false },
        { "Double", Core::ValueType::Double, 8, false },
    };
    for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); ++i) {
        if (finalType != specs[i].name) continue;
        if (rawLen < specs[i].width) return false;

        if (specs[i].vt == Core::ValueType::Float ||
            specs[i].vt == Core::ValueType::Double) {
            // 先按字节序装配整数位型, 再 memcpy 到浮点 (位型等值搬运)
            if (specs[i].vt == Core::ValueType::Float) {
                const uint32_t bits = Core::FromBytesU32(Core::ByteView(raw, 4), bo);
                float f;
                memcpy(&f, &bits, 4);
                out.type = Core::ValueType::Float;
                out.d = f;                       // 加宽存放 (无损)
            } else {
                const uint64_t bits = Core::FromBytesU64(Core::ByteView(raw, 8), bo);
                double d;
                memcpy(&d, &bits, 8);
                out.type = Core::ValueType::Double;
                out.d = d;
            }
            return true;
        }

        // 整数: 走 Core 装配函数 — 完整支持 4 种字节序
        // (BigEndian / LittleEndian / WordBigByteLittle=CDAB / WordLittleByteBig=BADC)
        uint64_t u = 0;
        if (specs[i].width == 2) {
            u = Core::FromBytesU16(Core::ByteView(raw, 2), bo);
        } else if (specs[i].width == 4) {
            u = Core::FromBytesU32(Core::ByteView(raw, 4), bo);
        } else {
            u = Core::FromBytesU64(Core::ByteView(raw, 8), bo);
        }
        if (specs[i].isSigned) {
            int64_t s = static_cast<int64_t>(u << (64 - specs[i].width * 8));
            s >>= (64 - specs[i].width * 8);     // 符号扩展
            out.type = specs[i].vt;
            out.i = s;
        } else {
            out.type = specs[i].vt;
            out.u = u;
        }
        return true;
    }
    return false;   // 未知 finalType
}

} // namespace

Core::ByteOrder ResponseParser::ResolveByteOrder(
    const Core::ProtocolConfig& protocol,
    const Core::Optional<Core::ByteOrder>& tagOverride) {

    if (tagOverride.has_value()) return tagOverride.value();
    if (protocol.dataByteOrder.has_value()) return protocol.dataByteOrder.value();
    return Core::ByteOrder::BigEndian;
}

bool ResponseParser::CheckCondition(const Core::ByteView& response,
                                    const std::string& validCondition) {
    if (validCondition.empty()) return true;
    return CheckValidCondition(response, validCondition);
}

Core::Expected<Core::TagValue> ResponseParser::Parse(const Core::ByteView& response,
                                                     const Core::ResponseParserConfig& config,
                                                     const Core::TagDefinition& tag,
                                                     Core::ByteOrder byteOrder) {
    // ── 有效条件校验 ──
    if (!config.validCondition.empty() &&
        !CheckValidCondition(response, config.validCondition)) {
        return Core::Unexpected(Core::Error::Code::InvalidResponse,
                                "validCondition 不满足", config.validCondition);
    }

    // ── 数据区定位 ──
    int start = config.dataStartIndex;
    if (start < 0 || static_cast<size_t>(start) > response.size) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                "dataStartIndex 越界");
    }

    // 数据区字节数 = tag.variables["ByteCount"] (由协议 outputs.derivedLength 派生后注入).
    //   跨协议统一字节单位: 引擎不假定"寄存器=2 字节" — Modbus tag 声明 ByteCount=N×2, S7 声明 N.
    //   未声明 ByteCount 时不假定任何宽度, 走下方 rawLen==0 回退 → 取剩余全量 (数据区起点至帧尾).
    size_t offset = static_cast<size_t>(start);
    size_t rawLen = 0;  // 0 = 未声明 ByteCount, 于下方回退为剩余全量
    {
        auto vit = tag.variables.find(Core::ByteCountVariableName());
        if (vit != tag.variables.end()) rawLen = static_cast<size_t>(vit->second);
    }

    // Bool 位提取偏移 — 直接读标签一等字段 tag.bitOffset (0-7, -1 = 未声明);
    //   未声明走 raw[0]!=0 整寄存器真假. 防御: >7 视为配置错误
    //   (正常应由 ConfigDeepValidator 规则13 拦截).
    int bitOffset = tag.bitOffset;
    if (bitOffset > 7) {
        return Core::Unexpected(Core::Error::Code::TypeConversionError,
                                "bitOffset 超出 0-7", tag.finalType);
    }
    if (rawLen == 0 || offset + rawLen > response.size) {
        rawLen = response.size - offset;                          // 回退: 取剩余全量
    }

    // ── 类型转换 ──
    Core::TagValue tv;
    tv.tagName = tag.name;
    tv.deviceId = tag.deviceId;
    tv.timestamp = NowMs();
    tv.rawData.assign(response.data + offset, response.data + offset + rawLen);

    if (!FillTypedValue(response.data + offset, rawLen,
                        tag.finalType, byteOrder, bitOffset, tv.typedValue)) {
        return Core::Unexpected(Core::Error::Code::TypeConversionError,
                                "finalType 不支持或数据不足", tag.finalType);
    }

    tv.quality = Core::QualityCode::Good;
    return tv;
}

}} // namespace MyProt::Engine
// src/Service/src/ResponsePreview.cpp
// Response parse preview — runs Engine::ResponseParser over caller-supplied bytes, using
// the operation's own responseParser config. No protocol knowledge lives here: every
// offset / condition / type decision comes from the parsed ProtocolConfig, exactly like
// Gateway::TagReader does at runtime.

#include "MyProt/Service/ResponsePreview.hpp"

#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Engine/ResponseParser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace MyProt { namespace Service {

namespace {

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// "03 04 00 2A" / "0304002A" / commas tolerated; odd digit count or non-hex → false.
bool ParseHexBytes(const std::string& text, std::vector<unsigned char>* out) {
    std::string digits;
    digits.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',') continue;
        if (HexDigit(c) < 0) return false;
        digits.push_back(c);
    }
    if (digits.empty() || digits.size() % 2 != 0) return false;
    out->clear();
    out->reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        out->push_back(static_cast<unsigned char>(
            (HexDigit(digits[i]) << 4) | HexDigit(digits[i + 1])));
    }
    return true;
}

std::string HexDump(const std::vector<unsigned char>& bytes) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i) out.push_back(' ');
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0x0F]);
    }
    return out;
}

std::string ByteOrderName(Core::ByteOrder order) {
    switch (order) {
        case Core::ByteOrder::BigEndian:         return "BigEndian";
        case Core::ByteOrder::LittleEndian:      return "LittleEndian";
        case Core::ByteOrder::WordBigByteLittle: return "WordBigByteLittle";
        case Core::ByteOrder::WordLittleByteBig: return "WordLittleByteBig";
    }
    return "BigEndian";
}

bool ParseByteOrderName(const std::string& text, Core::ByteOrder* out) {
    if (text == "BigEndian")         { *out = Core::ByteOrder::BigEndian; return true; }
    if (text == "LittleEndian")      { *out = Core::ByteOrder::LittleEndian; return true; }
    if (text == "WordBigByteLittle") { *out = Core::ByteOrder::WordBigByteLittle; return true; }
    if (text == "WordLittleByteBig") { *out = Core::ByteOrder::WordLittleByteBig; return true; }
    return false;
}

const char* ValueTypeName(Core::ValueType t) {
    switch (t) {
        case Core::ValueType::ByteArray: return "ByteArray";
        case Core::ValueType::UInt16:    return "UInt16";
        case Core::ValueType::Int16:     return "Int16";
        case Core::ValueType::UInt32:    return "UInt32";
        case Core::ValueType::Int32:     return "Int32";
        case Core::ValueType::UInt64:    return "UInt64";
        case Core::ValueType::Int64:     return "Int64";
        case Core::ValueType::Float:     return "Float";
        case Core::ValueType::Double:    return "Double";
        case Core::ValueType::Bool:      return "Bool";
        case Core::ValueType::String:    return "String";
        case Core::ValueType::Empty:     return "Empty";
    }
    return "Empty";
}

const char* QualityName(Core::QualityCode q) {
    switch (q) {
        case Core::QualityCode::Good:      return "Good";
        case Core::QualityCode::Bad:       return "Bad";
        case Core::QualityCode::Uncertain: return "Uncertain";
    }
    return "Uncertain";
}

void FillValueInto(const Core::TypedValue& tv, std::string* type, bool* hasValue,
                   std::string* text, bool* hasNumeric, double* numeric) {
    *type = ValueTypeName(tv.type);
    *hasValue = (tv.type != Core::ValueType::Empty);
    *hasNumeric = false;
    *numeric = 0.0;
    char buf[64];
    switch (tv.type) {
        case Core::ValueType::UInt16:
        case Core::ValueType::UInt32:
        case Core::ValueType::UInt64:
            *text = std::to_string(tv.u);
            *hasNumeric = true;
            *numeric = static_cast<double>(tv.u);
            break;
        case Core::ValueType::Int16:
        case Core::ValueType::Int32:
        case Core::ValueType::Int64:
            *text = std::to_string(tv.i);
            *hasNumeric = true;
            *numeric = static_cast<double>(tv.i);
            break;
        case Core::ValueType::Float:
        case Core::ValueType::Double:
            std::snprintf(buf, sizeof(buf), "%.10g", tv.d);
            *text = buf;
            *hasNumeric = true;
            *numeric = tv.d;
            break;
        case Core::ValueType::Bool:
            *text = tv.b ? "true" : "false";
            break;
        case Core::ValueType::String:
            *text = tv.str;
            break;
        case Core::ValueType::ByteArray:
            *text = HexDump(tv.bytes);
            break;
        case Core::ValueType::Empty:
            break;
    }
}

void FillValue(const Core::TypedValue& tv, ResponsePreviewResult* out) {
    FillValueInto(tv, &out->valueType, &out->hasValue, &out->valueText,
                  &out->hasNumeric, &out->numeric);
}

/// Rule 13 parity with SemanticChecker: converters only make sense for numeric types.
bool IsNumericFinalType(const std::string& ft) {
    return ft == "UInt16" || ft == "Int16" || ft == "UInt32" || ft == "Int32"
        || ft == "UInt64" || ft == "Int64" || ft == "Float" || ft == "Double";
}

/// Same lookup rule as the trial render preview: map key first, then op.name.
const Core::OperationConfig* FindOperation(
    const Core::ProtocolConfig& proto, const std::string& name) {

    std::unordered_map<std::string, Core::OperationConfig>::const_iterator hit =
        proto.operations.find(name);
    if (hit != proto.operations.end()) return &hit->second;
    for (std::unordered_map<std::string, Core::OperationConfig>::const_iterator it =
             proto.operations.begin(); it != proto.operations.end(); ++it) {
        if (it->second.name == name) return &it->second;
    }
    return 0;
}

} // namespace

ResponsePreviewResult::ResponsePreviewResult()
    : ok(false), conditionChecked(false), conditionOk(true)
    , dataStartIndex(0), frameLength(0), rawLength(0)
    , hasValue(false), hasNumeric(false), numeric(0.0), timestamp(0)
    , hasPreValue(false), hasPreNumeric(false), preNumeric(0.0) {}

ResponsePreviewResult PreviewProtocolResponse(const ResponsePreviewRequest& req) {
    ResponsePreviewResult out;

    // ── 1. Parse + deep-validate the candidate protocol text (same entry as --validate) ──
    const Core::Expected<ProtocolValidationOutcome> parsed =
        ConfigValidator::validateAndParseProtocolJson(req.protocolJson);
    if (!parsed.has_value()) {
        out.errors.push_back(parsed.error().message);
        return out;
    }
    const Core::ProtocolConfig& proto = parsed.value().proto;
    out.protocolName = proto.protocolName;
    for (std::size_t i = 0; i < parsed.value().result.errors.size(); ++i) {
        out.errors.push_back(parsed.value().result.errors[i]);
    }
    out.warnings = parsed.value().result.warnings;

    for (std::unordered_map<std::string, Core::OperationConfig>::const_iterator it =
             proto.operations.begin(); it != proto.operations.end(); ++it) {
        out.operations.push_back(it->first);
    }
    std::sort(out.operations.begin(), out.operations.end());
    if (req.operation.empty()) {
        out.ok = out.errors.empty();
        if (out.errors.empty()) {
            out.warnings.push_back("未指定 operation: 仅返回操作清单");
        }
        return out;
    }
    if (!out.errors.empty()) return out;   // an invalid config would produce a misleading value

    // ── 2. Locate the operation ──
    const Core::OperationConfig* op = FindOperation(proto, req.operation);
    if (!op) {
        std::string available;
        for (std::size_t i = 0; i < out.operations.size(); ++i) {
            if (i) available += ", ";
            available += out.operations[i];
        }
        out.errors.push_back("操作 \"" + req.operation + "\" 不存在 (可用: " + available + ")");
        return out;
    }
    out.operation = op->name.empty() ? req.operation : op->name;

    // ── 3. Response bytes ──
    std::vector<unsigned char> frame;
    if (!ParseHexBytes(req.hex, &frame)) {
        out.errors.push_back("响应 hex 非法: 须为偶数个十六进制数字 (空格/逗号可忽略)");
        return out;
    }
    out.frameLength = frame.size();

    // ── 4. Synthesize the tag-like inputs ResponseParser consumes ──
    if (req.bitOffset < -1 || req.bitOffset > 7) {
        out.errors.push_back("bitOffset 须为 -1 (不提取) 或 0..7");
        return out;
    }
    Core::TagDefinition tag;
    tag.name = "Preview";
    tag.deviceId = "preview";
    tag.finalType = req.finalType.empty() ? "UInt16" : req.finalType;
    tag.bitOffset = req.bitOffset;
    if (req.byteCount > 0) {
        tag.variables[Core::ByteCountVariableName()] = static_cast<uint32_t>(req.byteCount);
    }

    // Tag-level converter chain (v5 C1). A preview has no tags.json entry, so the caller
    // supplies the chain; validation mirrors ProtocolJsonParser / SemanticChecker rule 13 so
    // the preview cannot report a value the loader would have rejected.
    if (!req.converters.empty()) {
        if (!IsNumericFinalType(tag.finalType)) {
            out.errors.push_back("converters 仅支持数值型 finalType, 当前: \""
                + tag.finalType + "\" (规则13)");
            return out;
        }
        for (std::size_t i = 0; i < req.converters.size(); ++i) {
            const Core::ScaleConverter& c = req.converters[i];
            if (!std::isfinite(c.k) || !std::isfinite(c.b)) {
                out.errors.push_back("converters[" + std::to_string(i)
                    + "] 的 k/b 须为有限数值");
                return out;
            }
            if (c.k == 0.0) {
                out.errors.push_back("converters[" + std::to_string(i)
                    + "].k 不可为 0 (值域塌缩)");
                return out;
            }
        }
        tag.converters = req.converters;
        out.converters = req.converters;
    }

    Core::Optional<Core::ByteOrder> tagOrder;
    if (!req.byteOrder.empty()) {
        Core::ByteOrder parsedOrder = Core::ByteOrder::BigEndian;
        if (!ParseByteOrderName(req.byteOrder, &parsedOrder)) {
            out.errors.push_back("byteOrder 非法: " + req.byteOrder
                + " (可选 BigEndian | LittleEndian | WordBigByteLittle | WordLittleByteBig)");
            return out;
        }
        tagOrder = parsedOrder;
    }
    const Core::ByteOrder byteOrder = Engine::ResponseParser::ResolveByteOrder(proto, tagOrder);
    out.byteOrder = ByteOrderName(byteOrder);

    // ── 5. validCondition (reported separately so the UI can tell "frame rejected" from "parse failed") ──
    const Core::ByteView view(&frame[0], frame.size());
    out.conditionExpr = op->responseParser.validCondition;
    out.conditionChecked = !out.conditionExpr.empty();
    out.conditionOk = Engine::ResponseParser::CheckCondition(view, out.conditionExpr);
    if (out.conditionChecked && !out.conditionOk) {
        out.warnings.push_back("validCondition 不满足: " + out.conditionExpr);
    }

    // ── 6. Data-region bounds (mirror ResponseParser's own checks, but explain them) ──
    out.dataStartIndex = op->responseParser.dataStartIndex;
    if (out.dataStartIndex < 0
        || static_cast<std::size_t>(out.dataStartIndex) > frame.size()) {
        out.errors.push_back("dataStartIndex " + std::to_string(out.dataStartIndex)
            + " 越界 (响应仅 " + std::to_string(frame.size()) + " 字节)");
        return out;
    }
    if (req.byteCount > 0) {
        const long available = static_cast<long>(frame.size()) - out.dataStartIndex;
        if (req.byteCount > available) {
            out.warnings.push_back("ByteCount " + std::to_string(req.byteCount)
                + " 超出数据区可用 " + std::to_string(available) + " 字节, 将退化为剩余全量");
        }
    }

    // ── 7. Parse through the gateway's own implementation ──
    Engine::ResponseParser parser;

    // With a converter chain, parse once without it so the UI can show "raw -> engineering".
    if (!tag.converters.empty()) {
        Core::TagDefinition plain = tag;
        plain.converters.clear();
        const Core::Expected<Core::TagValue> pre =
            parser.Parse(view, op->responseParser, plain, byteOrder);
        if (pre.has_value()) {
            FillValueInto(pre.value().typedValue, &out.preValueType, &out.hasPreValue,
                          &out.preValueText, &out.hasPreNumeric, &out.preNumeric);
        }
    }

    const Core::Expected<Core::TagValue> parsedValue =
        parser.Parse(view, op->responseParser, tag, byteOrder);
    if (!parsedValue.has_value()) {
        std::string msg = "解析失败: " + parsedValue.error().message;
        if (!parsedValue.error().context.empty()) {
            msg += " (" + parsedValue.error().context + ")";
        }
        out.errors.push_back(msg);
        return out;
    }
    const Core::TagValue& tv = parsedValue.value();
    out.rawLength = tv.rawData.size();
    out.rawHex = HexDump(tv.rawData);
    FillValue(tv.typedValue, &out);
    out.quality = QualityName(tv.quality);
    out.timestamp = static_cast<long long>(tv.timestamp);

    out.ok = out.errors.empty();
    return out;
}

}} // namespace MyProt::Service

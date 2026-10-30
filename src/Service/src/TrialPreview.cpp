// src/Service/src/TrialPreview.cpp
// Trial render preview — assembles the frame through the same RequestBuilder pipeline as
// Gateway::TagReader::WriteBytes (CollectStaticVariables -> op overrides -> derivedLength
// injection -> AutoComputeProvider -> BuildBytes), so the preview bytes equal what the
// gateway would put on the wire for the same variables.
//
// This file deliberately contains no protocol knowledge: every offset/length decision comes
// from the parsed ProtocolConfig, exactly like the runtime path.

#include "MyProt/Service/TrialPreview.hpp"

#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Config.hpp"   // ExpectedLengthFieldValue (render-side length math)
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace MyProt { namespace Service {

namespace {

/// Payload used when the caller does not supply a {Name:raw} value (deterministic pattern).
std::string DummyPayloadHex(std::size_t bytes) {
    std::string hex;
    static const char* kDigits = "0123456789ABCDEF";
    for (std::size_t i = 0; i < bytes; ++i) {
        const unsigned char v = static_cast<unsigned char>((i % 255) + 1);
        if (!hex.empty()) hex.push_back(' ');
        hex.push_back(kDigits[v >> 4]);
        hex.push_back(kDigits[v & 0x0F]);
    }
    return hex;
}

/// Count the bytes encoded in a hex string ("01 02" / "0102"). Returns false on invalid input.
bool CountHexBytes(const std::string& text, std::size_t* bytes) {
    std::size_t digits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (!std::isxdigit(c)) return false;
        ++digits;
    }
    if (digits % 2 != 0) return false;
    *bytes = digits / 2;
    return true;
}

/// All {Name:raw} placeholder names in template order (deduplicated).
std::vector<std::string> RawPlaceholderNames(const std::vector<std::string>& tpl) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < tpl.size(); ++i) {
        const std::string& el = tpl[i];
        if (el.size() < 6 || el[0] != '{' || el[el.size() - 1] != '}') continue;
        if (el.compare(el.size() - 5, 5, ":raw}") != 0) continue;
        const std::string name = el.substr(1, el.size() - 6);
        if (seen.insert(name).second) names.push_back(name);
    }
    return names;
}

std::string HexDump(const Core::Bytes& frame) {
    static const char* kDigits = "0123456789ABCDEF";
    std::string out;
    out.reserve(frame.size() * 3);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        if (i) out.push_back(' ');
        out.push_back(kDigits[frame[i] >> 4]);
        out.push_back(kDigits[frame[i] & 0x0F]);
    }
    return out;
}

std::string AsciiDump(const Core::Bytes& frame) {
    std::string out;
    out.reserve(frame.size());
    for (std::size_t i = 0; i < frame.size(); ++i) {
        const unsigned char c = frame[i];
        out.push_back((c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '.');
    }
    return out;
}

std::string ByteOrderName(Core::ByteOrder order) {
    return order == Core::ByteOrder::BigEndian ? "BigEndian" : "LittleEndian";
}

/// Length slot value decoded per framing byte order (same rule as TrialRenderer).
bool ReadLengthSlot(const Core::Bytes& frame, const Core::LengthFieldConfig& lf,
                    unsigned long long* value) {
    const std::size_t need = static_cast<std::size_t>(lf.lengthFieldOffset)
                           + static_cast<std::size_t>(lf.lengthFieldLength);
    if (frame.size() < need) return false;
    const uint8_t* p = frame.data() + lf.lengthFieldOffset;
    const bool bigEndian = (lf.byteOrder == Core::ByteOrder::BigEndian);
    unsigned long long v = 0;
    switch (lf.lengthFieldLength) {
        case 1: v = p[0]; break;
        case 2: v = bigEndian
            ? ((static_cast<unsigned long long>(p[0]) << 8) | p[1])
            : ((static_cast<unsigned long long>(p[1]) << 8) | p[0]);
            break;
        case 4: v = bigEndian
            ? ((static_cast<unsigned long long>(p[0]) << 24)
               | (static_cast<unsigned long long>(p[1]) << 16)
               | (static_cast<unsigned long long>(p[2]) << 8) | p[3])
            : ((static_cast<unsigned long long>(p[3]) << 24)
               | (static_cast<unsigned long long>(p[2]) << 16)
               | (static_cast<unsigned long long>(p[1]) << 8) | p[0]);
            break;
        default: return false;   // unsupported width — validation rule 3 already rejects it
    }
    *value = v;
    return true;
}

} // namespace

TrialPreviewResult::TrialPreviewResult()
    : ok(false), frameLength(0)
    , hasLengthField(false), lengthFieldOffset(0), lengthFieldLength(0)
    , lengthFieldValue(0), lengthFieldExpected(0), lengthFieldOk(false) {}

TrialPreviewResult PreviewProtocolOperation(const TrialPreviewRequest& req) {
    TrialPreviewResult out;

    // ── 1. Parse + deep-validate the candidate protocol text ──
    //   Config comments/BOM are handled inside validateAndParseProtocolJson (same entry as
    //   --validate), so the preview and the gateway accept exactly the same text.
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

    // Available operations (sorted) — returned even when the request names none.
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
    if (!out.errors.empty()) {
        // An invalid config would render misleading bytes — refuse to continue.
        return out;
    }

    // ── 2. Locate the operation (map key first, then op.name) ──
    const Core::OperationConfig* op = 0;
    std::unordered_map<std::string, Core::OperationConfig>::const_iterator oit =
        proto.operations.find(req.operation);
    if (oit != proto.operations.end()) {
        op = &oit->second;
    } else {
        // VS2015: keep the reverse scan simple and explicit.
        for (std::unordered_map<std::string, Core::OperationConfig>::const_iterator it =
                 proto.operations.begin(); it != proto.operations.end(); ++it) {
            if (it->second.name == req.operation) { op = &it->second; break; }
        }
    }
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

    // ── 3. Variable pool: protocol static < operation static < caller overrides ──
    std::unordered_map<std::string, uint32_t> variables =
        Engine::RequestBuilder::CollectStaticVariables(proto);
    std::map<std::string, TrialPreviewVariable> resolved;
    for (std::unordered_map<std::string, uint32_t>::const_iterator it = variables.begin();
         it != variables.end(); ++it) {
        TrialPreviewVariable v;
        v.name = it->first;
        v.value = it->second;
        v.hasValue = true;
        v.source = "protocol";
        resolved[it->first] = v;
    }
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
             op->inputs.begin(); it != op->inputs.end(); ++it) {
        if (!it->second.isStatic() || !it->second.value.has_value()) continue;
        const uint32_t value = it->second.value.value();
        variables[it->first] = value;
        TrialPreviewVariable v;
        v.name = it->first;
        v.value = value;
        v.hasValue = true;
        v.source = "operation";
        resolved[it->first] = v;
    }
    for (std::unordered_map<std::string, unsigned int>::const_iterator it =
             req.variables.begin(); it != req.variables.end(); ++it) {
        const uint32_t value = it->second;
        variables[it->first] = value;
        TrialPreviewVariable v;
        v.name = it->first;
        v.value = value;
        v.hasValue = true;
        v.source = "override";
        resolved[it->first] = v;
    }
    // Engine contract variables: the gateway fills them from the tag; a preview synthesizes
    // sane defaults only when nothing else provided them.
    if (variables.find(Core::StartByteAddressVariableName()) == variables.end()) {
        variables[Core::StartByteAddressVariableName()] = 0;
        TrialPreviewVariable v;
        v.name = Core::StartByteAddressVariableName();
        v.value = 0;
        v.hasValue = true;
        v.source = "default";
        resolved[v.name] = v;
        out.warnings.push_back("未提供 " + v.name + ", 试渲染按 0 处理");
    }
    if (variables.find(Core::ByteCountVariableName()) == variables.end()) {
        variables[Core::ByteCountVariableName()] = 2;
        TrialPreviewVariable v;
        v.name = Core::ByteCountVariableName();
        v.value = 2;
        v.hasValue = true;
        v.source = "default";
        resolved[v.name] = v;
        out.warnings.push_back("未提供 " + v.name + ", 试渲染按 2 处理");
    }

    // ── 4. Raw payloads ({Name:raw}) and their total byte count ──
    const Engine::TemplateLayout layout =
        Engine::RequestBuilder::BuildTemplateLayout(op->requestTemplate);
    const std::vector<std::string> rawNames = RawPlaceholderNames(op->requestTemplate);
    std::unordered_map<std::string, std::string> rawVars;
    std::size_t totalRawBytes = 0;
    for (std::size_t i = 0; i < rawNames.size(); ++i) {
        const std::string& name = rawNames[i];
        std::unordered_map<std::string, std::string>::const_iterator given =
            req.rawVariables.find(name);
        std::string hex;
        if (given != req.rawVariables.end() && !given->second.empty()) {
            hex = given->second;
            std::size_t bytes = 0;
            if (!CountHexBytes(hex, &bytes)) {
                out.errors.push_back("载荷 \"" + name + "\" 不是合法 hex (需偶数个十六进制字符)");
                return out;
            }
            totalRawBytes += bytes;
        } else {
            hex = DummyPayloadHex(req.payloadBytes);
            totalRawBytes += req.payloadBytes;
            char note[128];
            std::snprintf(note, sizeof(note),
                          "未提供载荷 \"%s\", 试渲染使用 %u 字节占位 (01 02 03 ...)",
                          name.c_str(), static_cast<unsigned>(req.payloadBytes));
            out.warnings.push_back(note);
        }
        rawVars[name] = hex;
    }

    // ── 5. derivedLength injection (same call as TrialRenderer / TagReader) ──
    Engine::RequestBuilder::InjectDerivedLengthVariables(
        variables, proto.outputs, op->outputs, totalRawBytes, layout);
    std::set<std::string> derivedNames;
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
             proto.outputs.begin(); it != proto.outputs.end(); ++it) {
        if (it->second.isDerivedLength()) derivedNames.insert(it->first);
    }
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
             op->outputs.begin(); it != op->outputs.end(); ++it) {
        if (it->second.isDerivedLength()) derivedNames.insert(it->first);
    }
    for (std::set<std::string>::const_iterator it = derivedNames.begin();
         it != derivedNames.end(); ++it) {
        std::unordered_map<std::string, uint32_t>::const_iterator vit = variables.find(*it);
        TrialPreviewVariable v;
        v.name = *it;
        if (vit != variables.end()) {
            v.value = vit->second;
            v.hasValue = true;
            v.source = "derived";
        } else {
            v.source = "derived";
            out.errors.push_back("派生长度 \"" + *it + "\" 求值失败 (expr 语法错误或引用了未知变量)");
        }
        resolved[*it] = v;
    }

    // ── 6. autoCompute declarations: resolved per render, so only provenance is reported ──
    const std::string autoJson = Engine::RequestBuilder::MergeOpAutoComputeJson(proto, *op);
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
             proto.inputs.begin(); it != proto.inputs.end(); ++it) {
        if (it->second.source != "auto") continue;
        TrialPreviewVariable v;
        v.name = it->first;
        v.hasValue = false;
        v.source = "auto";
        resolved[it->first] = v;
    }
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
             op->inputs.begin(); it != op->inputs.end(); ++it) {
        if (it->second.source != "auto") continue;
        TrialPreviewVariable v;
        v.name = it->first;
        v.hasValue = false;
        v.source = "auto";
        resolved[it->first] = v;
    }

    // ── 7. Render through the real builder ──
    Engine::AutoComputeProvider autoProv;
    if (!autoJson.empty()) autoProv.DeclareJson(autoJson);

    Engine::RequestBuilder builder;
    const Core::Expected<Core::Bytes> built =
        builder.BuildBytes(*op, variables, rawVars, proto.varAliasMap, autoProv);
    if (!built.has_value()) {
        out.errors.push_back("试渲染失败: " + built.error().message);
        return out;
    }
    const Core::Bytes& frame = built.value();
    out.frameHex = HexDump(frame);
    out.ascii = AsciiDump(frame);
    out.frameLength = frame.size();

    // ── 8. Length-slot cross-check (framing semantics vs rendered bytes) ──
    const Core::LengthFieldConfig& lf = proto.framing.lengthField;
    if (proto.framing.type == Core::FramingType::LengthField && lf.lengthFieldLength > 0) {
        out.hasLengthField = true;
        out.lengthFieldOffset = lf.lengthFieldOffset;
        out.lengthFieldLength = lf.lengthFieldLength;
        out.lengthFieldByteOrder = ByteOrderName(lf.byteOrder);
        unsigned long long slot = 0;
        if (ReadLengthSlot(frame, lf, &slot)) {
            const long long total = static_cast<long long>(frame.size());
            const long long expected = Core::ExpectedLengthFieldValue(lf, total);
            out.lengthFieldValue = static_cast<long long>(slot);
            out.lengthFieldExpected = expected;
            out.lengthFieldOk = (out.lengthFieldValue == expected);
            if (!out.lengthFieldOk) {
                char msg[256];
                std::snprintf(msg, sizeof(msg),
                              "帧长一致性失败: 长度槽位值 %lld != 期望 %lld "
                              "(渲染总长 %lldB); 模板固定段或成帧配置不一致",
                              out.lengthFieldValue, out.lengthFieldExpected, total);
                out.errors.push_back(msg);
            }
        } else {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                          "渲染帧 (%uB) 短于长度槽位要求 (offset %d + width %d)",
                          static_cast<unsigned>(frame.size()),
                          lf.lengthFieldOffset, lf.lengthFieldLength);
            out.errors.push_back(msg);
        }
    }

    out.ok = out.errors.empty();
    for (std::map<std::string, TrialPreviewVariable>::const_iterator it = resolved.begin();
         it != resolved.end(); ++it) {
        out.variables.push_back(it->second);
    }
    return out;
}

}} // namespace MyProt::Service

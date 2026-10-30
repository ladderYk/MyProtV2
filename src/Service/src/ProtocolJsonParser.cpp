// src/Service/src/ProtocolJsonParser.cpp
// 配置 JSON → POCO 解析（protocols/*.json / tags.json 根文档）
// 拆分自 ConfigDeepValidator.cpp（P2-1）：本 TU 只做结构解析，字段取值规则由
// SchemaChecker / SemanticChecker 负责。文法工具经 ConfigInternals.hpp 供其余 TU 共用。

#include "ConfigInternals.hpp"

#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "MyProt/Core/TemplateGrammar.hpp" // 模板文法唯一实现 (Engine/Simulation 共用)

#include <nlohmann/json.hpp>

#include "MyProt/Engine/MiniExpression.hpp"   // 语法校验: 与运行时同一解析器
#include "MyProt/Service/ConfigValidator.hpp"  // isValidHexLiteral 单一实现

namespace MyProt { namespace Service {

namespace detail {

using nlohmann::json;

// ──────────────────── 基础工具 ────────────────────

bool IsHexChar(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentChar(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

// ──────────────────── 模板占位符 (§3.2) ────────────────────
// 文法: {Name:Xn} (定宽十六进制) | {Name:raw} (变长字节注入).
// 校验和走声明式 (inputs.source=auto strategy=crc); 不允许 {Name:algo:Xn} 这类模板内联文法
// inputs.source=auto strategy=crc + params.algo (crc16-modbus/crc16-ccitt/crc32),
// 派生长度走 outputs 的 derivedLength。
//
// 字面量解析/占位符拆分/宽度推导的唯一实现在 Core/TemplateGrammar
// (Engine 渲染、Simulation 反相匹配与本文件共用), 此处不再保留本地副本。

/// {Name:raw} 是 P1 A 变长字节注入标记; 与 Xn 定宽十六进制并列
/// (ADR-0007 §3 销账项; Config_Schema §3.2)
bool IsRawSpec(const std::string& s) {
    return s == "raw";
}

// ──────────────────── JSON 可选字段读取 ────────────────────
// 缺失字段不报错 (取 POCO 默认值); null 等同缺失 (取默认值);
// 存在但类型非法 → err 非空并返回 false。
bool OptStr(const json& j, const char* key, std::string& out, std::string& err) {
    if (!j.contains(key)) return true;
    const json& v = j.at(key);
    if (v.is_null()) { out.clear(); return true; }            // null → 默认空串
    if (!v.is_string()) { err = std::string(key) + " 类型非法 (须为字符串)"; return false; }
    out = v.get<std::string>();
    return true;
}

bool OptInt(const json& j, const char* key, int& out, std::string& err) {
    if (!j.contains(key)) return true;
    const json& v = j.at(key);
    if (v.is_null()) { return true; }                          // null → 保持当前值
    if (!v.is_number_integer()) { err = std::string(key) + " 类型非法 (须为整数)"; return false; }
    out = v.get<int>();
    return true;
}

bool OptUInt16(const json& j, const char* key, uint16_t& out, std::string& err) {
    int tmp = out;
    if (!OptInt(j, key, tmp, err)) return false;
    if (tmp < 0 || tmp > 65535) { err = std::string(key) + " 超出 uint16 范围"; return false; }
    out = static_cast<uint16_t>(tmp);
    return true;
}

bool OptUInt32(const json& j, const char* key, uint32_t& out, std::string& err) {
    if (!j.contains(key)) return true;
    const json& v = j.at(key);
    if (!v.is_number_integer()) { err = std::string(key) + " 类型非法 (须为非负整数)"; return false; }
    const int64_t tmp = v.get<int64_t>();
    if (tmp < 0 || tmp > 0xFFFFFFFFLL) { err = std::string(key) + " 超出 uint32 范围"; return false; }
    out = static_cast<uint32_t>(tmp);
    return true;
}

bool OptBool(const json& j, const char* key, bool& out, std::string& err) {
    if (!j.contains(key)) return true;
    const json& v = j.at(key);
    if (!v.is_boolean()) { err = std::string(key) + " 类型非法 (须为布尔)"; return false; }
    out = v.get<bool>();
    return true;
}

// ──────────────────── simulation 段 ────────────────────

bool IsValidSimTemplateLine(const std::string& raw) {
    std::size_t b = 0, e = raw.size();
    while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    const std::string s = raw.substr(b, e - b);
    if (s.empty()) return true;
    if (s[0] == '{') {
        if (s.size() < 2 || s[s.size() - 1] != '}') return false;
        const std::string inner = s.substr(1, s.size() - 2);
        if (inner == "data") return true;
        if (inner.compare(0, 4, "req:") != 0 || inner.size() < 5) return false;
        bool sawDigit = false;
        for (std::size_t i = 4; i < inner.size(); ++i) {
            const char c = inner[i];
            if (c == ':') continue;
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            sawDigit = true;
        }
        return sawDigit;
    }
    return ConfigValidator::isValidHexLiteral(s);
}

bool ParseSimulationDocImpl(const json& doc,
                            Core::SimulationConfig& sim, std::string& err) {
    if (!doc.is_object()) { err = "simulation 须为对象"; return false; }

    if (!OptUInt16(doc, "listenPort", sim.listenPort, err)) return false;
    if (!OptInt(doc, "registerCount", sim.registerCount, err)) return false;

    if (doc.contains("initialValues")) {
        const json& iv = doc.at("initialValues");
        if (!iv.is_object()) { err = "initialValues 须为对象 (地址 -> 值)"; return false; }
        for (json::const_iterator it = iv.begin(); it != iv.end(); ++it) {
            if (!it.value().is_number_integer()) {
                err = "initialValues[" + it.key() + "] 须为整数";
                return false;
            }
            const int64_t v = it.value().get<int64_t>();
            if (v < 0 || v > 65535) {
                err = "initialValues[" + it.key() + "] 须在 [0,65535]";
                return false;
            }
            sim.initialValues[it.key()] = static_cast<uint32_t>(v);
        }
    }

    if (doc.contains("operations")) {
        const json& ops = doc.at("operations");
        if (!ops.is_object()) { err = "operations 须为对象 (操作名 -> 仿真行为)"; return false; }
        for (json::const_iterator it = ops.begin(); it != ops.end(); ++it) {
            if (!it.value().is_object()) {
                err = "operations." + it.key() + " 须为对象";
                return false;
            }
            const json& op = it.value();
            Core::SimOperationConfig soc;
            std::string fieldErr;
            if (!OptStr(op, "kind", soc.kind, fieldErr)) {
                err = "operations." + it.key() + "." + fieldErr;
                return false;
            }
            if (!OptStr(op, "addressVar", soc.addressVar, fieldErr)) {
                err = "operations." + it.key() + "." + fieldErr;
                return false;
            }
            if (!OptStr(op, "countVar", soc.countVar, fieldErr)) {
                err = "operations." + it.key() + "." + fieldErr;
                return false;
            }
            if (!OptInt(op, "dataOffset", soc.dataOffset, fieldErr)) {
                err = "operations." + it.key() + "." + fieldErr;
                return false;
            }
            if (op.contains("responseTemplate")) {
                const json& rt = op.at("responseTemplate");
                if (!rt.is_array()) {
                    err = "operations." + it.key() + ".responseTemplate 须为数组";
                    return false;
                }
                for (json::const_iterator t = rt.begin(); t != rt.end(); ++t) {
                    if (!t->is_string()) {
                        err = "operations." + it.key()
                              + ".responseTemplate 元素须为字符串";
                        return false;
                    }
                    const std::string line = t->get<std::string>();
                    if (!IsValidSimTemplateLine(line)) {
                        err = "operations." + it.key()
                              + ".responseTemplate 行既非 hex 字面量也非 {data}/{req:N:M} 占位符";
                        return false;
                    }
                    soc.responseTemplate.push_back(line);
                }
            }
            sim.operations[it.key()] = soc;
        }
    }
    return true;
}

// ──────────────────── 枚举字符串转换 ────────────────────

bool ByteOrderFromString(const std::string& s, Core::ByteOrder& out) {
    if (s == "BigEndian")          { out = Core::ByteOrder::BigEndian; return true; }
    if (s == "LittleEndian")       { out = Core::ByteOrder::LittleEndian; return true; }
    if (s == "WordBigByteLittle")  { out = Core::ByteOrder::WordBigByteLittle; return true; }
    if (s == "WordLittleByteBig")  { out = Core::ByteOrder::WordLittleByteBig; return true; }
    return false;
}

bool TransportTypeFromString(const std::string& s, Core::TransportType& out) {
    if (s == "Tcp")    { out = Core::TransportType::Tcp; return true; }
    if (s == "Tls")    { out = Core::TransportType::Tls; return true; }
    if (s == "Serial") { out = Core::TransportType::Serial; return true; }
    if (s == "Udp")    { out = Core::TransportType::Udp; return true; }
    return false;
}

bool FramingTypeFromString(const std::string& s, Core::FramingType& out) {
    if (s == "LengthField") { out = Core::FramingType::LengthField; return true; }
    if (s == "Fixed")       { out = Core::FramingType::Fixed; return true; }
    if (s == "Silence")     { out = Core::FramingType::Silence; return true; }
    if (s == "Message")     { out = Core::FramingType::Message; return true; }
    return false;
}

bool LengthFieldEncodingFromString(const std::string& s, Core::LengthFieldEncoding& out) {
    if (s == "Fixed")  { out = Core::LengthFieldEncoding::Fixed; return true; }
    if (s == "Varint") { out = Core::LengthFieldEncoding::Varint; return true; }
    return false;
}

/// headerFields: [{"name","bitOffset","bitWidth"}] (ADR-0021 §2.2).
/// Absent = no field extraction; an empty array is allowed and means the same.
bool ParseHeaderFieldsDoc(const json& j, std::vector<Core::HeaderFieldConfig>& out,
                          std::string& err) {
    if (!j.contains("headerFields")) return true;
    const json& arr = j.at("headerFields");
    if (!arr.is_array()) { err = "headerFields 须为对象数组"; return false; }
    out.clear();
    out.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        const json& e = arr[i];
        if (!e.is_object()) { err = "headerFields 元素须为对象"; return false; }
        Core::HeaderFieldConfig f;
        if (!OptStr(e, "name", f.name, err)
                || !OptInt(e, "bitOffset", f.bitOffset, err)
                || !OptInt(e, "bitWidth", f.bitWidth, err)) {
            return false;
        }
        out.push_back(f);
    }
    return true;
}

/// dispatch: {"field","rejectUnknown","cases":[{"value","type","name"}]}
/// (ADR-0021 §2.3). Absent = no dispatch; Core::Frame::type stays 0.
bool ParseDispatchDoc(const json& j, Core::DispatchConfig& out, std::string& err) {
    if (!j.contains("dispatch")) return true;
    const json& d = j.at("dispatch");
    if (!d.is_object()) { err = "dispatch 须为对象"; return false; }
    if (!OptStr(d, "field", out.field, err)
            || !OptBool(d, "rejectUnknown", out.rejectUnknown, err)) {
        return false;
    }
    out.cases.clear();
    if (!d.contains("cases")) return true;
    const json& arr = d.at("cases");
    if (!arr.is_array()) { err = "dispatch.cases 须为对象数组"; return false; }
    out.cases.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        const json& e = arr[i];
        if (!e.is_object()) { err = "dispatch.cases 元素须为对象"; return false; }
        Core::DispatchCaseConfig c;
        if (!e.contains("value")) { err = "dispatch.cases[].value 缺失"; return false; }
        const json& v = e.at("value");
        if (v.is_number_unsigned()) {
            c.value = v.get<uint64_t>();
        } else if (v.is_number_integer()) {
            const int64_t raw = v.get<int64_t>();
            if (raw < 0) { err = "dispatch.cases[].value 必须 ≥ 0"; return false; }
            c.value = static_cast<uint64_t>(raw);
        } else {
            err = "dispatch.cases[].value 须为整数";
            return false;
        }
        int typeValue = static_cast<int>(c.value);
        if (!OptInt(e, "type", typeValue, err)
                || !OptStr(e, "name", c.name, err)) {
            return false;
        }
        if (typeValue < 0) { err = "dispatch.cases[].type 必须 ≥ 0"; return false; }
        c.type = static_cast<uint32_t>(typeValue);
        out.cases.push_back(c);
    }
    return true;
}

/// repeat: {"countField","stride","offset","name","maxCount"} (ADR-0021 §2.4).
/// Absent or countField empty = no repeated-record extraction.
bool ParseRepeatDoc(const json& j, Core::RepeatConfig& out, std::string& err) {
    if (!j.contains("repeat")) return true;
    const json& r = j.at("repeat");
    if (!r.is_object()) { err = "repeat 须为对象"; return false; }
    return OptStr(r, "countField", out.countField, err)
        && OptInt(r, "stride", out.stride, err)
        && OptInt(r, "offset", out.offset, err)
        && OptStr(r, "name", out.name, err)
        && OptInt(r, "maxCount", out.maxCount, err);
}

bool ParityFromString(const std::string& s, Core::SerialTransportConfig::Parity& out) {
    if (s == "None") { out = Core::SerialTransportConfig::Parity::None; return true; }
    if (s == "Odd")  { out = Core::SerialTransportConfig::Parity::Odd; return true; }
    if (s == "Even") { out = Core::SerialTransportConfig::Parity::Even; return true; }
    return false;
}

bool StopBitsFromString(const std::string& s, Core::SerialTransportConfig::StopBits& out) {
    if (s == "One") { out = Core::SerialTransportConfig::StopBits::One; return true; }
    if (s == "Two") { out = Core::SerialTransportConfig::StopBits::Two; return true; }
    return false;
}

// ──────────────────── JSON → POCO 解析 ────────────────────
// 结构性错误 (判别值未知 / 类型非法) 通过 err 返回; 字段取值规则留给深度校验器。

bool ParseTransportDoc(const json& j, Core::TransportConfig& t, std::string& err) {
    std::string typeStr;
    if (!OptStr(j, "type", typeStr, err)) return false;
    if (!TransportTypeFromString(typeStr, t.type)) {
        err = "未知 transport.type: \"" + typeStr + "\"";
        return false;
    }

    bool ok = true;
    switch (t.type) {
        case Core::TransportType::Tcp:
            ok = OptUInt16(j, "defaultPort", t.tcp.defaultPort, err);
            break;
        case Core::TransportType::Udp:
            ok = OptUInt16(j, "defaultPort", t.udp.defaultPort, err);
            break;
        case Core::TransportType::Tls:
            ok = OptUInt16(j, "defaultPort", t.tls.defaultPort, err)
              && OptStr(j, "certFile", t.tls.certFile, err)
              && OptStr(j, "keyFile", t.tls.keyFile, err)
              && OptStr(j, "caFile", t.tls.caFile, err)
              && OptBool(j, "verifyServer", t.tls.verifyServer, err);
            break;
        case Core::TransportType::Serial: {
            std::string parityStr("None"), stopBitsStr("One");
            int dataBits = static_cast<int>(t.serial.dataBits);
            ok = OptStr(j, "portName", t.serial.portName, err)
              && OptUInt32(j, "baudRate", t.serial.baudRate, err)
              && OptInt(j, "dataBits", dataBits, err)
              && OptStr(j, "parity", parityStr, err)
              && OptStr(j, "stopBits", stopBitsStr, err)
              && ParityFromString(parityStr, t.serial.parity)
              && StopBitsFromString(stopBitsStr, t.serial.stopBits);
            if (ok) {
                if (dataBits < 5 || dataBits > 8) {
                    err = "Serial dataBits 取值须为 5~8";
                    ok = false;
                } else {
                    t.serial.dataBits = static_cast<uint8_t>(dataBits);
                }
            }
            break;
        }
        default:
            err = "transport.type 判别值越界";
            ok = false;
            break;
    }
    return ok;
}

bool ParseFramingDoc(const json& j, Core::FramingConfig& f, std::string& err) {
    std::string typeStr;
    if (!OptStr(j, "type", typeStr, err)) return false;
    if (!FramingTypeFromString(typeStr, f.type)) {
        err = "未知 framing.type: \"" + typeStr + "\"";
        return false;
    }

    bool ok = true;
    switch (f.type) {
        case Core::FramingType::LengthField: {
            std::string encoding;
            ok = OptInt(j, "lengthFieldOffset", f.lengthField.lengthFieldOffset, err)
              && OptInt(j, "lengthFieldLength", f.lengthField.lengthFieldLength, err)
              && OptBool(j, "lengthIncludesHeader", f.lengthField.lengthIncludesHeader, err)
              && OptInt(j, "headerLength", f.lengthField.headerLength, err)
              && OptInt(j, "lengthAdjustment", f.lengthField.lengthAdjustment, err)
              && OptInt(j, "maxFrameSize", f.lengthField.maxFrameSize, err)
              && OptStr(j, "lengthFieldEncoding", encoding, err)
              && OptInt(j, "varintMaxBytes", f.lengthField.varintMaxBytes, err)
              && ParseHeaderFieldsDoc(j, f.lengthField.headerFields, err)
              && ParseDispatchDoc(j, f.lengthField.dispatch, err)
              && ParseRepeatDoc(j, f.lengthField.repeat, err);
            if (ok && !encoding.empty()
                    && !LengthFieldEncodingFromString(encoding, f.lengthField.lengthEncoding)) {
                err = "未知 lengthFieldEncoding: \"" + encoding + "\" (Fixed / Varint)";
                ok = false;
            }
            if (ok && j.contains("byteOrder")) {
                std::string boStr;
                if (!OptStr(j, "byteOrder", boStr, err) ||
                    !ByteOrderFromString(boStr, f.lengthField.byteOrder)) {
                    if (err.empty()) err = "未知 byteOrder: \"" + boStr + "\"";
                    ok = false;
                }
            }
            break;
        }
        case Core::FramingType::Fixed:
            ok = OptInt(j, "fixedLength", f.fixed.fixedLength, err);
            break;
        case Core::FramingType::Silence:
            ok = OptInt(j, "charTimeUs", f.silence.charTimeUs, err)
              && OptInt(j, "frameGapUs", f.silence.frameGapUs, err)
              && OptInt(j, "maxFrameSize", f.silence.maxFrameSize, err);
            break;
        case Core::FramingType::Message:
            ok = OptInt(j, "maxFrameSize", f.message.maxFrameSize, err)
              && OptInt(j, "idFieldLength", f.message.idFieldLength, err);
            break;
        default:
            err = "framing.type 判别值越界";
            ok = false;
            break;
    }
    return ok;
}

/// §11: 设备级覆盖字段集须为全局块子集 (未知字段报名称错误)
bool ParseResilienceDoc(const json& j, Core::ResilienceConfig& res, std::string& err) {
    static const char* const kKnown[] = {
        "maxAttempts", "backoffBaseMs", "backoffMaxMs",
        "failureThreshold", "cooldownMs", "halfOpenProbes"
    };
    for (json::const_iterator it = j.begin(); it != j.end(); ++it) {
        bool known = false;
        for (size_t i = 0; i < sizeof(kKnown) / sizeof(kKnown[0]); ++i) {
            if (it.key() == kKnown[i]) { known = true; break; }
        }
        if (!known) { err = "resilience 存在未知字段: " + it.key(); return false; }
    }
    return OptInt(j, "maxAttempts", res.maxAttempts, err)
        && OptInt(j, "backoffBaseMs", res.backoffBaseMs, err)
        && OptInt(j, "backoffMaxMs", res.backoffMaxMs, err)
        && OptInt(j, "failureThreshold", res.failureThreshold, err)
        && OptInt(j, "cooldownMs", res.cooldownMs, err)
        && OptInt(j, "halfOpenProbes", res.halfOpenProbes, err);
}

/// §12 管理面块解析 (webRoot 为本机扩展字段, 不入 §12 校验范围)
bool ParseWebApiDoc(const json& j, Core::WebApiConfig& api, std::string& err) {
    return OptStr(j, "bindAddress", api.bindAddress, err)
        && OptStr(j, "certFile", api.certFile, err)
        && OptStr(j, "keyFile", api.keyFile, err)
        && OptBool(j, "requireAuth", api.requireAuth, err)
        && OptInt(j, "rateLimitRps", api.rateLimitRps, err)
        && OptInt(j, "rateLimitBurst", api.rateLimitBurst, err)
        && OptStr(j, "webRoot", api.webRoot, err);
}

/// 南向推送接入块 (§2.x, device-level): ingest.type selects the adapter branch.
bool ParseIngestDoc(const json& j, Core::IngestConfig& ing, std::string& err) {
    if (!j.is_object()) { err = "ingest 须为对象"; return false; }
    if (!j.contains("type") || !j.at("type").is_string()) {
        err = "ingest.type 须为字符串 (mqtt / tcp)";
        return false;
    }
    ing.type = j.at("type").get<std::string>();

    if (ing.type == "mqtt") {
        if (!j.contains("mqtt") || !j.at("mqtt").is_object()) {
            err = "ingest.type=mqtt 须含 mqtt 对象";
            return false;
        }
        const json& m = j.at("mqtt");
        Core::MqttIngestConfig& c = ing.mqtt;
        if (!OptStr(m, "host", c.host, err)
                || !OptUInt16(m, "port", c.port, err)
                || !OptStr(m, "clientId", c.clientId, err)
                || !OptInt(m, "keepAliveSec", c.keepAliveSec, err)
                || !OptBool(m, "cleanSession", c.cleanSession, err)
                || !OptInt(m, "qos", c.qos, err)
                || !OptInt(m, "reconnectMinMs", c.reconnectMinMs, err)
                || !OptInt(m, "reconnectMaxMs", c.reconnectMaxMs, err)
                || !OptInt(m, "maxSilenceMs", c.maxSilenceMs, err)
                || !OptStr(m, "topic", c.topic, err)
                || !OptStr(m, "tagFrom", c.tagFrom, err)
                || !OptInt(m, "tagTopicIndex", c.tagTopicIndex, err)
                || !OptStr(m, "tagField", c.tagField, err)
                || !OptStr(m, "valueField", c.valueField, err)
                || !OptStr(m, "tsField", c.tsField, err)) {
            return false;
        }
        if (m.contains("username")) {
            if (!m.at("username").is_string()) { err = "ingest.mqtt.username 须为字符串"; return false; }
            c.username = m.at("username").get<std::string>();
        }
        if (m.contains("password")) {
            if (!m.at("password").is_string()) { err = "ingest.mqtt.password 须为字符串"; return false; }
            c.password = m.at("password").get<std::string>();
        }
        return true;
    }

    if (ing.type == "tcp") {
        if (!j.contains("tcp") || !j.at("tcp").is_object()) {
            err = "ingest.type=tcp 须含 tcp 对象";
            return false;
        }
        const json& t = j.at("tcp");
        Core::TcpIngestConfig& c = ing.tcp;
        if (!OptStr(t, "bindAddress", c.bindAddress, err)
                || !OptUInt16(t, "port", c.port, err)
                || !OptInt(t, "maxSilenceMs", c.maxSilenceMs, err)
                || !OptStr(t, "tagField", c.tagField, err)
                || !OptStr(t, "valueField", c.valueField, err)
                || !OptStr(t, "tsField", c.tsField, err)) {
            return false;
        }
        if (t.contains("framing")) {
            if (!ParseFramingDoc(t.at("framing"), c.framing, err)) return false;
        }
        return true;
    }

    err = "ingest.type 取值非法: \"" + ing.type + "\" (仅 mqtt / tcp)";
    return false;
}

bool ParseTemplateArray(const json& j, std::vector<std::string>& out, std::string& err) {
    if (!j.is_array()) { err = "requestTemplate 须为字符串数组"; return false; }
    out.reserve(j.size());
    for (size_t i = 0; i < j.size(); ++i) {
        if (!j[i].is_string()) { err = "requestTemplate 元素须为字符串"; return false; }
        out.push_back(j[i].get<std::string>());
    }
    return true;
}

// ──────────────────── 长度偏移自检 ────────────────────
// 变长派生长度若落点为成帧长度字段槽位 (offset/length 与 framing.lengthField 一致),
// 其相对 payload 的常量偏移必须与模板固定字节布局一致, 防止改写模板后静默错帧.
int TemplateElementBytes(const std::string& el) {
    if (!el.empty() && el[0] == '{') {
        std::vector<std::string> parts;
        if (Core::TemplateGrammar::SplitPlaceholder(el, parts) && !parts.empty()) {
            const std::string& fmt = parts[parts.size() - 1];
            if (fmt == "raw")  return 0;
            {   // Xn/XnLE → n/2 字节 (与文法层同一解析)
                const int wb = Core::TemplateGrammar::WidthSpecBytes(fmt);
                if (wb > 0) return wb;
            }
        }
        return 0;
    }
    int n = 0; bool inTok = false;
    for (size_t i = 0; i < el.size(); ++i) {
        if (el[i] == ' ' || el[i] == '\t') { inTok = false; continue; }
        if (!inTok) { ++n; inTok = true; }
    }
    return n;
}
bool TemplateElementName(const std::string& el, std::string& name) {
    if (el.empty() || el[0] != '{') return false;
    std::vector<std::string> parts;
    if (!Core::TemplateGrammar::SplitPlaceholder(el, parts) || parts.empty()) return false;
    name = parts[0];
    return true;
}
// 尝试把 expr 归约为 "{<载荷名>:len}" / "{<载荷名>:len} ± C" 的常量偏移; 其它形式返回 false (跳过自检).
// 载荷名 = 模板 {Name:raw} 占位符名; expr 以 {name:len} 引用.
bool TryPayloadOffset(const std::string& expr, const std::string& payloadVarName,
                      long& off) {
    std::string s = expr;
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    if (s.compare(i, payloadVarName.size(), payloadVarName) != 0) return false;
    i += payloadVarName.size();
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    if (s.compare(i, 3, "len") != 0) return false;
    i += 3;
    if (i >= s.size() || s[i] != '}') return false;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i == s.size()) { off = 0; return true; }
    long sign = 0;
    if (s[i] == '+') sign = 1; else if (s[i] == '-') sign = -1; else return false;
    ++i;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    long v = 0; bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); ++i; any = true; }
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (!any || i != s.size()) return false;
    off = sign * v;
    return true;
}

/// strategy=derivedLength 的 expr 语法可行性检查
///   与运行时同一 MiniExpression 解析器做真实语法解析, 堵住
///         语法错误 (如 "payload)" / "payload +* 7"), 防启动期静默放行错帧.
///   expr 不限制基准变量名 (outputs 可引用 inputs);
///         引用名合法性 (标识符 ∈ inputs 已声明名, {name:len} 取字节长度) 交由解析后的
///         ValidateDerivedLengthExprDomain 校验 (此时输入名集合已知).
bool IsDerivedLengthExprValid(const std::string& expr) {
    try {
        MyProt::Engine::MiniExpression::Parser p(expr);
        p.Parse();
    } catch (...) {
        return false;
    }
    return true;
}

/// 收集 expr 中引用的标识符集 — 由 ValidateDerivedLengthExprDomain 内部
/// prop 感知扫描取代 (需区分 {Frame:fixed}/{name:offset}/{name:len}/裸名), 原函数删除。

/// 模板 {Name:raw} 占位符名集合 — 变长写载荷由模板自动识别, 无需 inputs 特殊声明.
std::set<std::string> CollectRawPlaceholderNames(
    const std::vector<std::string>& requestTemplate) {
    std::set<std::string> names;
    for (const auto& el : requestTemplate) {
        if (el.size() < 2 || el[0] != '{' || el[el.size() - 1] != '}') continue;
        const size_t colon = el.find(':');
        if (colon == std::string::npos) continue;
        const size_t end = el.find('}', colon);
        if (end == std::string::npos) continue;
        if (el.substr(colon + 1, end - colon - 1) != "raw") continue;
        names.insert(el.substr(1, colon - 1));
    }
    return names;
}

/// 模板全部占位符名集合 (ADR-0012 §1.1, 任意格式) —
/// {name:offset} 原语的合法引用域 (offset 引用模板占位符的首现偏移).
std::set<std::string> CollectTemplatePlaceholderNames(
    const std::vector<std::string>& requestTemplate) {
    std::set<std::string> names;
    for (const auto& el : requestTemplate) {
        if (el.size() < 2 || el[0] != '{' || el[el.size() - 1] != '}') continue;
        const size_t colon = el.find(':');
        if (colon == std::string::npos) continue;
        names.insert(el.substr(1, colon - 1));
    }
    return names;
}

/// outputs(derivedLength) 的 expr 引用域校验 (prop 感知).
///   {Frame:fixed}   → 保留原语, 恒合法 (ADR-0012 §1.1);
///   {Name:offset}   → Name 须为模板占位符名 (placeholderNames);
///   {Name:len}/裸名 → 旧行为: 同一作用域 inputs 已声明名 ∪ 模板 {Name:raw} 载荷名.
bool ValidateDerivedLengthExprDomain(
        const std::string& prefix,
        const std::unordered_map<std::string, Core::VariableConfig>& protoInputs,
        const std::unordered_map<std::string, Core::VariableConfig>* opInputs,
        const std::unordered_map<std::string, Core::VariableConfig>& outputs,
        const std::set<std::string>& rawNames,
        const std::set<std::string>& placeholderNames,
        std::string& err) {
    std::set<std::string> allowed;
    for (auto& kv : protoInputs) allowed.insert(kv.first);
    if (opInputs) for (auto& kv : *opInputs) allowed.insert(kv.first);
    for (std::set<std::string>::const_iterator it = rawNames.begin();
         it != rawNames.end(); ++it) allowed.insert(*it);

    for (auto& kv : outputs) {
        const Core::VariableConfig& v = kv.second;
        if (!v.isDerivedLength() || v.expr.empty()) continue;
        const std::string& expr = v.expr;
        const size_t n = expr.size();
        size_t i = 0;
        while (i < n) {
            const unsigned char c = static_cast<unsigned char>(expr[i]);
            if (c == '{') {
                size_t j = i + 1;
                while (j < n && (std::isalnum(static_cast<unsigned char>(expr[j]))
                                 || expr[j] == '_')) ++j;
                const std::string name = expr.substr(i + 1, j - i - 1);
                std::string prop;
                size_t k = j;
                if (k < n && expr[k] == ':') {
                    const size_t pstart = ++k;
                    while (k < n && (std::isalnum(static_cast<unsigned char>(expr[k]))
                                     || expr[k] == '_')) ++k;
                    prop = expr.substr(pstart, k - pstart);
                }
                const size_t close = expr.find('}', k);
                if (close == std::string::npos) { i = n; break; }
                                // prop 合法性校验 — 仅 len/offset/fixed 三种属性
                if (prop != Core::kPropLength && prop != Core::kPropOffset
                        && prop != Core::kPropFixed) {
                    err = prefix + ".outputs." + kv.first
                        + ".expr 使用了不支持的属性 \"" + prop + "\" "
                          "(仅支持 len/offset/fixed; "
                          "{name:len} 取变量字节宽度, {name:offset} 取占位符首现偏移, "
                          "{Frame:fixed} 取模板固定段总宽)";
                    return false;
                }
                if (prop == Core::kPropFixed && name == Core::kFramePrimitiveName) {
                    // 保留原语 — 恒合法
                } else if (prop == "offset") {
                    if (placeholderNames.find(name) == placeholderNames.end()
                            && allowed.find(name) == allowed.end()) {
                        err = prefix + ".outputs." + kv.first
                            + ".expr 的 {name:offset} 引用了模板中不存在的占位符 \""
                            + name + "\"";
                        return false;
                    }
                } else if (prop == "len") {
                    // {name:len} — name 须为 inputs 已声明名或模板 {Name:raw} 载荷名
                    if (allowed.find(name) == allowed.end()) {
                        err = prefix + ".outputs." + kv.first
                            + ".expr 的 {name:len} 引用了未声明的输入变量 \"" + name
                            + "\" (仅允许引用同一作用域 inputs 已声明名或模板 {Name:raw} 载荷名)";
                        return false;
                    }
                }
                i = close + 1;
            } else if (std::isalpha(c) || c == '_') {
                size_t j = i;
                while (j < n && (std::isalnum(static_cast<unsigned char>(expr[j]))
                                 || expr[j] == '_')) ++j;
                const std::string id = expr.substr(i, j - i);
                                // 浮点检测 — expr 中含 "." 提示浮点常量
                bool hasDot = false;
                for (size_t d = i; d < j; ++d) {
                    if (expr[d] == '.') { hasDot = true; break; }
                }
                if (hasDot) {
                    err = prefix + ".outputs." + kv.first
                        + ".expr 含浮点常量 \"" + id + "\" "
                          "(MiniExpression 仅支持整数运算, 浮点数将解析失败; "
                          "请改用整数, 或确认是否为误写)";
                    return false;
                }
                // 内建函数名 (hi8/lo8/hi16/lo16) 不是变量, 不参与声明域校验;
                // 仅当后面紧跟 '(' 时才按函数处理, 避免误放行同名变量
                size_t k = j;
                while (k < n && (expr[k] == ' ' || expr[k] == '\t')) ++k;
                const bool isBuiltinCall = (k < n && expr[k] == '('
                    && MyProt::Engine::MiniExpression::IsBuiltinFunction(id));
                if (!isBuiltinCall && allowed.find(id) == allowed.end()) {
                    err = prefix + ".outputs." + kv.first
                        + ".expr 引用了未声明的输入变量 \"" + id
                        + "\" (仅允许引用同一作用域 inputs 已声明名或模板 {Name:raw} 载荷名, "
                          "{name:len} 取字节长度, {Frame:fixed}/{name:offset} 为模板结构原语, "
                          "hi8/lo8/hi16/lo16 为内建函数)";
                    return false;
                }
                i = j;
            } else {
                ++i;
            }
        }
    }

    return true;
}

/// 同一作用域内 inputs 与 outputs 不得同名 — 防"一个名字串场".
bool CheckNoInputOutputNameConflict(
        const std::string& prefix,
        const std::unordered_map<std::string, Core::VariableConfig>& inputs,
        const std::unordered_map<std::string, Core::VariableConfig>& outputs,
        std::string& err) {
    for (auto& kv : outputs) {
        if (inputs.find(kv.first) != inputs.end()) {
            err = prefix + " 同名变量 \"" + kv.first
                + "\" 同时出现在 inputs 与 outputs (v1.17: 同一操作域内不得同名; "
                "跨操作允许同名不同角色)";
            return false;
        }
    }
    return true;
}

/// 输入声明于协议 inputs 段 (配置形态: inputs / outputs 两组).
/// 规则 14 判定"模板变量能否由协议供给": 协议 inputs 中 source=static 且有值 (运行时注入
/// 变量池) 或 source=auto (经 AutoComputeProvider 求值) 的名字均视为可在运行时解析,
/// 放行模板占位符; 仅静态无值 (原 hint / 纯 UI 展示) 不算供给, 模板引用即报未定义.
bool ProtocolResolvesInputVariable(const Core::ProtocolConfig& proto, const std::string& name) {
    std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
        proto.inputs.find(name);
    if (it == proto.inputs.end()) return false;
    if (it->second.isAuto()) return true;                       // auto 运行时求值 (可解析)
    return it->second.isStatic() && it->second.value.has_value(); // 静态有值才注入
}

/// 解析单个变量条目 (复用, 同时被 protocol/op 的 inputs 与 outputs 调用)
///   prefix 用于错误消息 (e.g. "inputs" / "operations.ReadCoils.outputs")
///   autoComputeOutJson 输出拼接好的 auto 段 JSON (供 Engine AutoComputeProvider.DeclareJson)
bool ParseOneVariableDoc(const std::string& prefix,
                         const std::string& vname,
                         const json& v,
                         Core::VariableConfig& out,
                         std::string& autoComputeOutJson,
                         std::string& err) {
    if (!v.is_object()) {
        err = prefix + "." + vname + " 须为对象 { source, ... }";
        return false;
    }
    if (!v.contains("source") || !v.at("source").is_string()) {
        err = prefix + "." + vname + " 缺失 source 字符串 (static / auto)";
        return false;
    }
    const std::string src = v.at("source").get<std::string>();
    // source 仅 static / auto 两种取值; 其余 (含历史 "hint") 直接报错
    if (src != "static" && src != "auto") {
        err = prefix + "." + vname + " source 须为 static / auto (实际: \"" + src + "\")";
        return false;
    }
        // 保留名冲突校验 — 用户变量名不可与引擎保留名撞名
    //   模板原语: {Frame:fixed} (Frame 为保留名)
    //   expr 魔法变量: __frameLen, __frameEnd
    //   属性名: len/offset/fixed (仅作 prop 名, 但变量名同名语义混淆)
        // 保留名清单唯一来源在 Core (禁止此处与下方别名检查各写一份同一集合)
    if (Core::IsFrameReservedName(vname)) {
        err = prefix + "." + vname
              + " 变量名与引擎保留名冲突 ("
              "Frame = {Frame:fixed} 模板原语; "
              "__frameLen/__frameEnd = expr 内置魔法变量; "
              "len/offset/fixed = {Name:prop} 属性名); "
              "请改用其他变量名";
        return false;
    }
    out.source = src;

    // static 段 — value 可选: 有 value → 运行时注入 ctx.variables; 无 value → 纯 UI 展示 (原 hint)
    if (out.source == "static") {
        if (v.contains("value")) {
            const json& val = v.at("value");
            if (!val.is_number_integer()) {
                err = prefix + "." + vname + ".value 须为整数 (uint32_t)";
                return false;
            }
            int64_t v64 = val.get<int64_t>();
            if (v64 < 0 || v64 > 0xFFFFFFFFLL) {
                err = prefix + "." + vname + ".value 超出 uint32_t 范围";
                return false;
            }
            out.value = static_cast<uint32_t>(v64);
        }
    }
    // 自动计算 (策略 + 参数)
    else if (src == "auto") {
        if (!v.contains("strategy") || !v.at("strategy").is_string()) {
            err = prefix + "." + vname + " source=auto 须含 strategy 字符串";
            return false;
        }
        out.strategy = v.at("strategy").get<std::string>();
        static const char* const kKnownStrategies[] = {
            "autoIncrement", "frameSlice", "expr", "crc", "derivedLength"
        };
        bool known = false;
        for (size_t i = 0; i < sizeof(kKnownStrategies)/sizeof(*kKnownStrategies); ++i) {
            if (out.strategy == kKnownStrategies[i]) { known = true; break; }
        }
        if (!known) {
            err = prefix + "." + vname + " strategy \"" + out.strategy + "\" 不在内置策略中 "
                "(autoIncrement/frameSlice/expr/crc/derivedLength)";
            return false;
        }
        if (out.strategy == "derivedLength") {
                        // 派生长度: 唯一表达 = expr (载荷字节数的算术表达式). 求值时机在
            //   WriteBytes (依赖载荷字节数) → 不进 autoComputeJson; 由 TagReader
            //   参数层预解析注入.
                        //   不使用 kind 字段 (payload/registers/payloadPlus4/...) — 出现即报错引导改 expr.
            if (v.contains("kind")) {
                err = prefix + "." + vname + " strategy=derivedLength 的 kind 字段已移除 (v1.16); "
                    "请直接写 expr (e.g. kind:payload → expr:\"Payload\" / kind:registers → expr:\"Payload / 2\")";
                return false;
            }
            const std::string expr = v.value("expr", "");
            if (expr.empty()) {
                err = prefix + "." + vname + " strategy=derivedLength 须声明 expr (载荷字节数的算术表达式)";
                return false;
            }
            if (!IsDerivedLengthExprValid(expr)) {
                err = prefix + "." + vname + " expr \"" + expr + "\" 语法非法 (须为合法算术表达式, "
                    "可引用 inputs 已声明名, {name:len} 取字节长度)";
                return false;
            }
            out.expr = expr;
        } else if (v.contains("params")) {
            const json& pms = v.at("params");
            if (!pms.is_object()) {
                err = prefix + "." + vname + ".params 须为对象";
                return false;
            }
                        // strategy=crc 的 params.algo 必填且须为受支持算法.
            //   引擎侧 ExecCrc 已不再提供缺省算法 (原缺省 crc16-modbus 把协议选择
            //   烘焙进 Engine); 校验在此兜住, 避免等到运行期才失败.
            if (out.strategy == "crc") {
                if (!pms.contains("algo") || !pms.at("algo").is_string()) {
                    err = prefix + "." + vname + ".params.algo 必填 (strategy=crc), "
                          "取值: crc16-modbus / crc16-ccitt / crc32";
                    return false;
                }
                const std::string algo = pms.at("algo").get<std::string>();
                if (algo != "crc16-modbus" && algo != "crc16-ccitt" && algo != "crc32") {
                    err = prefix + "." + vname + ".params.algo \"" + algo
                          + "\" 不在受支持算法中 (crc16-modbus / crc16-ccitt / crc32)";
                    return false;
                }
            }
            // 原文 dump() 留给 Engine (避免 Engine 引入 nlohmann/json.hpp 依赖)
            out.paramsJson = pms.dump();
            // 同步拼接到 autoComputeOutJson — 累加 {VarName: {strategy, params}} 形式
            if (!autoComputeOutJson.empty()) autoComputeOutJson += ",";
            autoComputeOutJson += "\"" + vname + "\":{" + v.at("strategy").dump() + ",";
            autoComputeOutJson += "\"params\":" + pms.dump() + "}";
        } else {
                        // strategy=crc 不带 params 非法 (algo 无从取得)
            if (out.strategy == "crc") {
                err = prefix + "." + vname + " strategy=crc 须提供 params.algo "
                      "(crc16-modbus / crc16-ccitt / crc32)";
                return false;
            }
            // 无 params (e.g. 无 seed 的 autoIncrement, 取默认值 0)
            if (!autoComputeOutJson.empty()) autoComputeOutJson += ",";
            autoComputeOutJson += "\"" + vname + "\":{" + v.at("strategy").dump() + "}";
        }
    }
        // 展示元信息 (任意 source 都允许; 无值 static 仅在此有意义)
    if (v.contains("label"))        out.label        = v.at("label").get<std::string>();
    if (v.contains("unit"))         out.unit         = v.at("unit").get<std::string>();
    if (v.contains("placeholder"))  out.placeholder  = v.at("placeholder").get<std::string>();
    if (v.contains("enum")) {
        const json& en = v.at("enum");
        if (!en.is_array()) {
            err = prefix + "." + vname + ".enum 须为数组";
            return false;
        }
        // 元素形态 (Config_Schema §3.2): 裸数字 (简写, 显示十进制字面量)
        //   或 { "value": N, "label": "..." } 对象; 混合合法. 空数组 = 无候选 (合法).
        for (size_t i = 0; i < en.size(); ++i) {
            const json& e = en[i];
            Core::VariableConfig::EnumMember m;
            if (e.is_number_integer()) {
                const int64_t v64 = e.get<int64_t>();
                if (v64 < 0 || v64 > 0xFFFFFFFFLL) {
                    err = prefix + "." + vname + ".enum[] 超出 uint32_t";
                    return false;
                }
                m.value = static_cast<uint32_t>(v64);
                m.label = std::to_string(m.value);
            } else if (e.is_object()) {
                if (!e.contains("value") || !e.at("value").is_number_integer()) {
                    err = prefix + "." + vname + ".enum[] 对象须含整数 value";
                    return false;
                }
                const int64_t v64 = e.at("value").get<int64_t>();
                if (v64 < 0 || v64 > 0xFFFFFFFFLL) {
                    err = prefix + "." + vname + ".enum[].value 超出 uint32_t";
                    return false;
                }
                m.value = static_cast<uint32_t>(v64);
                if (e.contains("label")) {
                    if (!e.at("label").is_string()) {
                        err = prefix + "." + vname + ".enum[].label 须为字符串";
                        return false;
                    }
                    m.label = e.at("label").get<std::string>();
                } else {
                    m.label = std::to_string(m.value);
                }
            } else {
                err = prefix + "." + vname + ".enum[] 须为整数或 {value,label} 对象";
                return false;
            }
            for (size_t k = 0; k < out.enumValues.size(); ++k) {
                if (out.enumValues[k].value == m.value) {
                    err = prefix + "." + vname + ".enum 候选值重复: "
                          + std::to_string(m.value);
                    return false;
                }
            }
            out.enumValues.push_back(m);
        }
    }
    return true;
}

/// 通用 inputs 段解析 — 只允许 source=static / source=auto(非 derivedLength);
///   派生输出 (derivedLength) 属 outputs 段, 出现在 inputs 即报错.
bool ParseInputsDoc(const std::string& prefix, const json& doc,
                    std::unordered_map<std::string, Core::VariableConfig>& inputs,
                    std::string& err) {
    if (!doc.contains("inputs")) return true;
    const json& vs = doc.at("inputs");
    if (!vs.is_object()) {
        err = prefix + ".inputs 须为对象 ({ VarName: { source, ... } })";
        return false;
    }
    std::string acJson;   // 仅用于 ParseOneVariableDoc 累加; 运行时从 inputs 重建
    for (auto it = vs.begin(); it != vs.end(); ++it) {
        Core::VariableConfig vcfg;
        if (!ParseOneVariableDoc(prefix + ".inputs", it.key(), it.value(), vcfg, acJson, err)) return false;
        if (vcfg.isDerivedLength()) {
            err = prefix + ".inputs." + it.key()
                + " strategy=derivedLength 应声明在 outputs 段 (派生长度是输出, v1.17 拆分)";
            return false;
        }
        inputs[it.key()] = vcfg;
    }
    return true;
}

/// 通用 outputs 段解析 — 只允许 source=auto strategy=derivedLength.
bool ParseOutputsDoc(const std::string& prefix, const json& doc,
                     std::unordered_map<std::string, Core::VariableConfig>& outputs,
                     std::string& err) {
    if (!doc.contains("outputs")) return true;
    const json& vs = doc.at("outputs");
    if (!vs.is_object()) {
        err = prefix + ".outputs 须为对象 ({ VarName: { source, ... } })";
        return false;
    }
    std::string dummy;
    for (auto it = vs.begin(); it != vs.end(); ++it) {
        Core::VariableConfig vcfg;
        if (!ParseOneVariableDoc(prefix + ".outputs", it.key(), it.value(), vcfg, dummy, err)) return false;
        if (!vcfg.isDerivedLength()) {
            err = prefix + ".outputs." + it.key()
                + " 仅允许 strategy=derivedLength (派生输出); 静态值/自动增量等请放入 inputs 段 (v1.17 拆分)";
            return false;
        }
        outputs[it.key()] = vcfg;
    }
    return true;
}

bool ParseOperationDoc(const json& j, Core::OperationConfig& op, std::string& err) {
    if (!j.contains("requestTemplate")) {
        err = "操作 " + op.name + " 缺失 requestTemplate";
        return false;
    }
    if (!ParseTemplateArray(j.at("requestTemplate"), op.requestTemplate, err)) return false;

    if (j.contains("responseParser")) {
        const json& rp = j.at("responseParser");
        if (!rp.is_object()) { err = "操作 " + op.name + " responseParser 须为对象"; return false; }
        if (!OptStr(rp, "validCondition", op.responseParser.validCondition, err)
                || !OptInt(rp, "dataStartIndex", op.responseParser.dataStartIndex, err)
                || !OptStr(rp, "dataLengthExpr", op.responseParser.dataLengthExpr, err)) {
            return false;
        }
        // v1.28: validCondition 加载期语法校验 (与运行时同一 MiniExpression 解析器).
        //   堵住"写了多条件不报错、运行时静默失败"盲区 — 语法错误在此报 Error, 不放行到运行时.
        if (!op.responseParser.validCondition.empty()) {
            try {
                MyProt::Engine::MiniExpression::Parser p(op.responseParser.validCondition);
                p.Parse();
            } catch (const MyProt::Engine::MiniExpression::Error& e) {
                err = "操作 " + op.name + " validCondition 语法错误: " + e.what();
                return false;
            } catch (...) {
                err = "操作 " + op.name + " validCondition 语法错误";
                return false;
            }
        }
    }
        // 操作语义标注 (可选; 空 = 未标注, "read" | "write" 由校验器核对)
    if (!OptStr(j, "kind", op.kind, err)) return false;
        // 只发操作 (可选, 缺省 true = 请求-应答).
    //   false = 发送后不读取也不等待应答, 仅适用于设备确实不应答的写类操作;
    //   读标签引用与 readBack 组合由 SemanticChecker / TagReader 拒绝.
    if (!OptBool(j, "expectResponse", op.expectResponse, err)) return false;
        // 操作级 inputs / outputs 覆盖 (操作级声明覆盖协议级同名条目).
    //   输入/输出同域同名冲突在此校验; 输出引用域 (需协议级 inputs) 由 ParseProtocolDocImpl
    //   在 operations 全量解析后统一校验.
    if (!ParseInputsDoc(op.name, j, op.inputs, err)) return false;
    if (!ParseOutputsDoc(op.name, j, op.outputs, err)) return false;
    if (!CheckNoInputOutputNameConflict(op.name, op.inputs, op.outputs, err)) return false;
    // 注: 单次请求-应答超时统一由 device.requestTimeoutMs 配置 (2026-08-24 收敛),
    //     协议操作不再解析 timeoutMs (旧配置残留字段被忽略)。
    return true;
}

/// 协议文档 → POCO; defaultVer 用于缺失 schemaVersion 时的回填
bool ParseProtocolDocImpl(const json& doc, int defaultVer,
                          Core::ProtocolConfig& p, std::string& err) {
    p.schemaVersion = defaultVer;

    if (!OptStr(doc, "protocolName", p.protocolName, err)) return false;
    if (!OptInt(doc, "schemaVersion", p.schemaVersion, err)) return false;

    if (!doc.contains("transport") || !doc.at("transport").is_object()) {
        err = "缺失 transport 或类型非法 (须为对象)";
        return false;
    }
    if (!ParseTransportDoc(doc.at("transport"), p.transport, err)) return false;

    if (!doc.contains("framing") || !doc.at("framing").is_object()) {
        err = "缺失 framing 或类型非法 (须为对象)";
        return false;
    }
    if (!ParseFramingDoc(doc.at("framing"), p.framing, err)) return false;

    // v1.x 增: 协议级数据字节序 (可选); 标签未显式声明 byteOrder 时回退至此
    if (doc.contains("dataByteOrder")) {
        std::string boStr;
        Core::ByteOrder bo = Core::ByteOrder::BigEndian;
        if (!OptStr(doc, "dataByteOrder", boStr, err)
                || !ByteOrderFromString(boStr, bo)) {
            if (err.empty()) err = "未知 dataByteOrder: \"" + boStr + "\"";
            return false;
        }
        p.dataByteOrder = bo;
    }

        // 写路径操作名 (可选; 缺省取 Modbus 约定默认值, 见 ProtocolConfig 构造)
        //   载荷变量由模板 {Name:raw} 占位符自动识别 (不存在 inputs payloadLength 声明).
        // 协议级 writeOperation / writeBytesOperation 不属于 Schema — 写声明点唯一位于标签层

        // 标签按地址邻近合并的最大字节跨度 (TagGrouper::CoalesceAdjacent 消费).
    //   协议族"单次读取上限"的通用表达; 缺省值见 Core::kDefaultMaxSpanBytes.
    if (!OptInt(doc, "maxSpanBytes", p.maxSpanBytes, err)) return false;
    if (p.maxSpanBytes <= 0) {
        err = "maxSpanBytes 须 > 0 (单位: 字节), 得到: "
              + std::to_string(p.maxSpanBytes);
        return false;
    }

        // 变量别名映射 (alias → internal name).
        //   可映射的契约名仅 2 个 — 只保留引擎真正查表读取的跨协议字节单位.
    //     其余旧名不再作为契约:
    //       StartAddress / RegisterCount — 协议 JSON outputs 自行声明的派生名 (引擎不读);
    //       WriteValue                   — 写路径默认值常量 (标签 writeVariable 可覆盖);
    //       BitOffset                    — 标签一等字段 TagDefinition::bitOffset.
    //   校验: alias 名须为合法标识符, 不可与 inputs/outputs 中已有键冲突, 不可映射到非契约名.
    if (doc.contains("variableAliases")) {
        const json& va = doc.at("variableAliases");
        if (!va.is_object()) { err = "variableAliases 须为对象"; return false; }
        // 契约名单一真源: 取自 Core 的约定函数 (避免此处再复制一份字面量).
        const std::string kStartName = Core::StartByteAddressVariableName();
        const std::string kCountName = Core::ByteCountVariableName();
        std::set<std::string> kAllowedSet;
        kAllowedSet.insert(kStartName);
        kAllowedSet.insert(kCountName);
        for (json::const_iterator it = va.begin(); it != va.end(); ++it) {
            std::string alias = it.key();
            if (!IsIdentStart(alias[0])) {
                err = "variableAliases 键名须以字母开头: " + alias;
                return false;
            }
            for (size_t i = 1; i < alias.size(); ++i) {
                if (!IsIdentChar(alias[i])) {
                    err = "variableAliases 键名须为标识符: " + alias;
                    return false;
                }
            }
            std::string internal;
            if (!OptStr(va, alias.c_str(), internal, err)) return false;
            if (kAllowedSet.find(internal) == kAllowedSet.end()) {
                err = "variableAliases 值仅可为契约名之一 ("
                      + kStartName + " / " + kCountName + "), 得到: " + internal;
                return false;
            }
            if (alias == internal) {
                err = "variableAliases 中别名与内部名相同无意义: " + alias;
                return false;
            }
                        // 别名不可与引擎保留名冲突 (Frame/{Frame:fixed}、__frameLen/__frameEnd expr 魔法变量)
                        // 保留名清单唯一来源在 Core
            if (Core::IsFrameReservedName(alias)) {
                err = "variableAliases 别名 \"" + alias
                      + "\" 与引擎保留名冲突 ("
                      "Frame = {Frame:fixed} 模板原语; "
                      "__frameLen/__frameEnd = expr 内置魔法变量; "
                      "len/offset/fixed = {Name:prop} 属性名); 请改用其他别名";
                return false;
            }
            // 别名不可与 inputs/outputs 中已有键冲突 (防 alias 与同名变量并存)
            if (p.inputs.find(alias) != p.inputs.end() ||
                p.outputs.find(alias) != p.outputs.end()) {
                err = "variableAliases 别名与 inputs/outputs 中已有键冲突: " + alias;
                return false;
            }
            p.varAliasMap[alias] = internal;
        }
    }

        // 协议级 inputs / outputs 段 (可选); 配置中不存在单段 protocol.variables.
    //   inputs : static 值/UI 提示 + auto{autoIncrement,frameSlice,expr,crc}
    //   outputs: source=auto strategy=derivedLength 的派生输出 (引用 inputs 名或模板 raw 载荷名, {name:len} 取字节长度)
        //   metadata.placeholderHints 不属于当前 Schema。
    if (!ParseInputsDoc("protocol", doc, p.inputs, err)) return false;
    if (!ParseOutputsDoc("protocol", doc, p.outputs, err)) return false;
    if (!CheckNoInputOutputNameConflict("protocol", p.inputs, p.outputs, err)) return false;
        // 协议级 outputs 引用域 — 在操作解析后执行 ({name:offset} 的合法引用域
    // 是全部操作的模板占位符名集, 须等操作就绪); 无 operations 时占位符集为空,
    // 检查仍然执行 (与旧行为一致)
    std::set<std::string> allPlaceholderNames;

    if (doc.contains("operations")) {
        const json& ops = doc.at("operations");
        if (!ops.is_object()) { err = "operations 须为对象 (map<操作名, OperationConfig>)"; return false; }
        for (json::const_iterator it = ops.begin(); it != ops.end(); ++it) {
            if (!it.value().is_object()) { err = "操作 " + it.key() + " 定义须为对象"; return false; }
            Core::OperationConfig op;
            op.name = it.key();
            if (!ParseOperationDoc(it.value(), op, err)) return false;
            p.operations.insert(std::make_pair(it.key(), op));
        }
                // 各操作的 outputs 引用域校验 (需协议级 inputs 全量就绪) —
                //   {name:offset} 的合法引用域 = 该操作模板占位符名集
        for (std::unordered_map<std::string, Core::OperationConfig>::iterator oi =
                p.operations.begin(); oi != p.operations.end(); ++oi) {
            if (!ValidateDerivedLengthExprDomain(
                    oi->first, p.inputs, &oi->second.inputs, oi->second.outputs,
                    CollectRawPlaceholderNames(oi->second.requestTemplate),
                    CollectTemplatePlaceholderNames(oi->second.requestTemplate),
                    err)) return false;
        }
        for (std::unordered_map<std::string, Core::OperationConfig>::iterator oi =
                p.operations.begin(); oi != p.operations.end(); ++oi) {
            std::set<std::string> names =
                CollectTemplatePlaceholderNames(oi->second.requestTemplate);
            allPlaceholderNames.insert(names.begin(), names.end());
        }
    }
    if (!ValidateDerivedLengthExprDomain("protocol", p.inputs, nullptr, p.outputs,
                                         {}, allPlaceholderNames, err)) return false;

    if (doc.contains("handshake")) {
        const json& hsArr = doc.at("handshake");
        if (!hsArr.is_array()) { err = "handshake 须为数组"; return false; }
        for (size_t i = 0; i < hsArr.size(); ++i) {
            const json& sj = hsArr[i];
            if (!sj.is_object()) { err = "handshake[" + std::to_string(i) + "] 须为对象"; return false; }
            Core::HandshakeStep step;
            if (!OptStr(sj, "name", step.name, err)
                    || !OptStr(sj, "validCondition", step.validCondition, err)
                    || !OptStr(sj, "sessionExtractExpr", step.sessionExtractExpr, err)
                    || !OptStr(sj, "sessionVariable", step.sessionVariable, err)
                    || !OptInt(sj, "timeoutMs", step.timeoutMs, err)) {
                return false;
            }
            if (sj.contains("requestTemplate")
                    && !ParseTemplateArray(sj.at("requestTemplate"), step.requestTemplate, err)) {
                return false;
            }
            if (sj.contains("framingOverride") && sj.at("framingOverride").is_object()) {
                Core::FramingConfig fo;
                if (!ParseFramingDoc(sj.at("framingOverride"), fo, err)) return false;
                step.framingOverride = fo;
            }
            p.handshake.push_back(step);
        }
    }

    // 协议级 simulation 段 (可选): 本协议的独立仿真实例; listenPort == 0 = 不启用.
    // 多协议各自声明即可同时运行多个仿真设备 (端口须互异, 由加载器校验).
    if (doc.contains("simulation")) {
        if (!ParseSimulationDocImpl(doc.at("simulation"), p.simulation, err)) {
            err = "simulation: " + err;
            return false;
        }
    }
    return true;
}

bool ParseRootDocImpl(const json& doc, int defaultVer,
                      Core::ConfigRoot& root, std::string& err) {
    root.schemaVersion = defaultVer;
    if (!OptInt(doc, "schemaVersion", root.schemaVersion, err)) return false;

    if (doc.contains("resilience")) {
        const json& rj = doc.at("resilience");
        if (!rj.is_object()) { err = "resilience 须为对象"; return false; }
        Core::ResilienceConfig res;
        if (!ParseResilienceDoc(rj, res, err)) return false;
        root.resilience = res;
    }

    if (doc.contains("webApi")) {
        const json& wj = doc.at("webApi");
        if (!wj.is_object()) { err = "webApi 须为对象"; return false; }
        Core::WebApiConfig api;
        if (!ParseWebApiDoc(wj, api, err)) return false;
        root.webApi = api;
    }

    // 设备级变量缺省表 (id → variables): 供标签解析时做 §4 回退合并
    std::map<std::string, std::map<std::string, uint32_t> > devVarsById;

    if (doc.contains("devices")) {
        const json& devs = doc.at("devices");
        if (!devs.is_array()) { err = "devices 须为数组"; return false; }
        for (size_t i = 0; i < devs.size(); ++i) {
            const json& dj = devs[i];
            if (!dj.is_object()) { err = "devices[" + std::to_string(i) + "] 须为对象"; return false; }
            Core::DeviceConfig dev;
            if (!OptStr(dj, "id", dev.id, err)
                    || !OptStr(dj, "protocol", dev.protocol, err)) {
                return false;
            }
            // username/password 为可选字符串 (Optional<string>)
            if (dj.contains("username")) {
                if (!dj.at("username").is_string()) { err = "username 须为字符串"; return false; }
                dev.username = dj.at("username").get<std::string>();
            }
            if (dj.contains("password")) {
                if (!dj.at("password").is_string()) { err = "password 须为字符串"; return false; }
                dev.password = dj.at("password").get<std::string>();
            }
            if (dj.contains("connection")) {
                const json& cj = dj.at("connection");
                if (!cj.is_object()) { err = "connection 须为对象"; return false; }
                if (!OptStr(cj, "host", dev.connection.host, err)
                        || !OptUInt16(cj, "port", dev.connection.port, err)
                        || !OptStr(cj, "portName", dev.connection.portName, err)
                        || !OptInt(cj, "timeoutMs", dev.connection.timeoutMs, err)) {
                    return false;
                }
            }
            // 设备级请求超时 (2026-08-24 收敛: 单次请求-应答唯一配置点)
            if (!OptInt(dj, "requestTimeoutMs", dev.requestTimeoutMs, err)) {
                return false;
            }
            if (dj.contains("resilience")) {
                const json& rj = dj.at("resilience");
                if (!rj.is_object()) { err = "设备 " + dev.id + " resilience 须为对象"; return false; }
                Core::ResilienceConfig res;
                if (!ParseResilienceDoc(rj, res, err)) return false;
                dev.resilience = res;
            }
            if (dj.contains("ingest")) {
                Core::IngestConfig ing;
                if (!ParseIngestDoc(dj.at("ingest"), ing, err)) {
                    err = "设备 " + dev.id + " " + err;
                    return false;
                }
                dev.ingest = ing;
            }
            // 设备级模板变量缺省 (§4): 可选 map<string,uint32>
            if (dj.contains("variables")) {
                const json& vj = dj.at("variables");
                if (!vj.is_object()) {
                    err = "设备 " + dev.id + " variables 须为对象";
                    return false;
                }
                for (json::const_iterator it = vj.begin(); it != vj.end(); ++it) {
                    uint32_t val = 0;
                    if (!OptUInt32(vj, it.key().c_str(), val, err)) return false;
                    dev.variables.insert(std::make_pair(it.key(), val));
                }
            }
            if (!dev.id.empty()) {
                devVarsById[dev.id] = dev.variables;
            }
            root.devices.push_back(dev);
        }
    }

    if (doc.contains("tags")) {
        const json& tags = doc.at("tags");
        if (!tags.is_array()) { err = "tags 须为数组"; return false; }
        for (size_t i = 0; i < tags.size(); ++i) {
            const json& tj = tags[i];
            if (!tj.is_object()) { err = "tags[" + std::to_string(i) + "] 须为对象"; return false; }
            Core::TagDefinition tag;
            if (!OptStr(tj, "name", tag.name, err)
                    || !OptStr(tj, "deviceId", tag.deviceId, err)
                    || !OptStr(tj, "operation", tag.operation, err)
                    // 展示元数据 (Web UI / 报表使用; 引擎不消费)
                    || !OptStr(tj, "label", tag.label, err)
                    || !OptStr(tj, "unit", tag.unit, err)
                    || !OptStr(tj, "finalType", tag.finalType, err)
                    || !OptInt(tj, "scanRateMs", tag.scanRateMs, err)
                                        // registerCount 顶层字段不存在; 走 variables.ByteCount (协议族字节单位)
                                        // reportMode / deadband 顶层字段不存在 — 消费侧无落点 (见 ROADMAP 上报过滤)
                                        // 写标签: direction/writeVariable/readBackTag (缺省走 TagDefinition 构造默认)
                    || !OptStr(tj, "direction", tag.direction, err)
                    || !OptStr(tj, "source", tag.source, err)
                    || !OptStr(tj, "writeVariable", tag.writeVariable, err)
                    || !OptStr(tj, "readBackTag", tag.readBackTag, err)
                                        // 标签级写能力: writeOperation + writeVariables (缺省 = 只读)
                    || !OptStr(tj, "writeOperation", tag.writeOperation, err)
                    || !OptStr(tj, "writeBytesOperation", tag.writeBytesOperation, err)
                                        // 位偏移为标签一等字段 (缺省 -1 = 未声明)
                    || !OptInt(tj, "bitOffset", tag.bitOffset, err)) {
                return false;
            }
            // push 标签不经过模板解析: 未显式声明时按 payload 原生类型推断, 且不参与轮询
            if (tag.source == "push") {
                if (!tj.contains("finalType")) tag.finalType = "Auto";
                if (!tj.contains("scanRateMs")) tag.scanRateMs = 0;
            }
                        // tag.address 不存在 — 无任何消费方 (引擎不解析、Schema/API 未登记)
            // 注: 请求-应答超时不在此解析, 统一由 device.requestTimeoutMs 决定 (2026-08-24 收敛)
            if (tj.contains("variables")) {
                const json& vj = tj.at("variables");
                if (!vj.is_object()) { err = "标签 " + tag.name + " variables 须为对象"; return false; }
                for (json::const_iterator it = vj.begin(); it != vj.end(); ++it) {
                    uint32_t val = 0;
                    if (!OptUInt32(vj, it.key().c_str(), val, err)) return false;
                    tag.variables.insert(std::make_pair(it.key(), val));
                }
            }
                        // 标签级写能力: 写请求专用变量覆盖 (可选 map<变量名, 整数>)
            if (tj.contains("writeVariables")) {
                const json& wv = tj.at("writeVariables");
                if (!wv.is_object()) { err = "标签 " + tag.name + " writeVariables 须为对象"; return false; }
                for (json::const_iterator it = wv.begin(); it != wv.end(); ++it) {
                    uint32_t val = 0;
                    if (!OptUInt32(wv, it.key().c_str(), val, err)) return false;
                    tag.writeVariables.insert(std::make_pair(it.key(), val));
                }
            }
            if (tj.contains("byteOrder")) {
                std::string boStr;
                Core::ByteOrder bo = Core::ByteOrder::BigEndian;
                if (!OptStr(tj, "byteOrder", boStr, err)
                        || !ByteOrderFromString(boStr, bo)) {
                    if (err.empty()) err = "标签 " + tag.name + " byteOrder 取值非法: \"" + boStr + "\"";
                    return false;
                }
                tag.byteOrder = bo;
            }
            if (tj.contains("coalesce")) {
                if (!OptBool(tj, "coalesce", tag.coalesce, err)) return false;
            }
            // v5 (C1): 读后换算链 — 数组, 每项 {kind:"scale", k:number, b:number}.
            //   解析期 fail-fast: kind 不支持 / k、b 缺失或非数值即拒绝加载.
            if (tj.contains("converters")) {
                if (!tj.at("converters").is_array()) {
                    err = "标签 " + tag.name + " converters 须为数组"; return false;
                }
                for (const auto& cj : tj.at("converters")) {
                    if (!cj.is_object() || !cj.contains("kind") || !cj.at("kind").is_string()) {
                        err = "标签 " + tag.name + " converters 项须为含 kind 的对象"; return false;
                    }
                    const std::string kind = cj.at("kind").get<std::string>();
                    if (kind != "scale") {
                        err = "标签 " + tag.name + " converters.kind 取值非法: \"" + kind
                            + "\" (当前仅支持 scale)";
                        return false;
                    }
                    Core::ScaleConverter conv;
                    if (!cj.contains("k") || !cj.at("k").is_number()
                            || !cj.contains("b") || !cj.at("b").is_number()) {
                        err = "标签 " + tag.name + " converters.scale 须含数值 k 与 b";
                        return false;
                    }
                    conv.k = cj.at("k").get<double>();
                    conv.b = cj.at("b").get<double>();
                    if (conv.k == 0.0) {
                        err = "标签 " + tag.name + " converters.scale.k 不可为 0 (值域塌缩)";
                        return false;
                    }
                    tag.converters.push_back(conv);
                }
            }
            // §4 设备级变量回退合并: map::insert 不覆盖已有键 → 标签显式声明优先
            std::map<std::string, std::map<std::string, uint32_t> >::const_iterator dv =
                devVarsById.find(tag.deviceId);
            if (dv != devVarsById.end()) {
                for (std::map<std::string, uint32_t>::const_iterator iv = dv->second.begin();
                        iv != dv->second.end(); ++iv) {
                    tag.variables.insert(std::make_pair(iv->first, iv->second));
                }
            }
            root.tags.push_back(tag);
        }
    }
    return true;
}

} // namespace detail

}} // namespace MyProt::Service

// src/Service/src/ConfigDeepValidator.cpp
// Config_Schema §7 深度校验器实现 — 十五项规则 + 版本门禁 (ADR-0005)
//                     + §11 韧性参数 + §12 管理面参数 + 跨文件协议引用
// 使用 ConfigValidator.hpp 的 ValidationResult (工程内唯一校验结果类型)。

#include "MyProt/Service/ConfigValidator.hpp"

#include <cctype>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include "MyProt/Engine/MiniExpression.hpp"   // 语法校验: 与运行时同一解析器

namespace MyProt { namespace Service {

namespace {

using nlohmann::json;

// ──────────────────── 基础工具 ────────────────────

std::wstring ToWide(const std::string& s) {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n) - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

bool FileExists(const std::string& path) {
    if (path.empty()) return false;
    return ::GetFileAttributesW(ToWide(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

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

bool IsFormatSpec(const std::string& s) {
    return s == "X2" || s == "X4" || s == "X8" || s == "X16";
}

/// {Name:raw} 是 P1 A 变长字节注入标记; 与 Xn 定宽十六进制并列
/// (ADR-0007 §3 销账项; Config_Schema §3.2)
bool IsRawSpec(const std::string& s) {
    return s == "raw";
}

/// 拆分 {Name:fmt} 的内部成分 (按 ':' 分段; 成分只允许标识字符)
bool SplitPlaceholder(const std::string& line, std::vector<std::string>& parts) {
    parts.clear();
    if (line.size() < 2 || line[0] != '{' || line[line.size() - 1] != '}') return false;

    const std::string inner = line.substr(1, line.size() - 2);
    std::string cur;
    for (size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (c == ':') {
            parts.push_back(cur);
            cur.clear();
        } else if (IsIdentChar(c)) {
            cur += c;
        } else {
            return false;
        }
    }
    parts.push_back(cur);

    if (parts.empty() || parts[0].empty()) return false;
    if (!IsIdentStart(parts[0][0])) return false;          // 名称: [A-Za-z_]\w*
    return parts.size() <= 3;
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
    return false;
}

bool FramingTypeFromString(const std::string& s, Core::FramingType& out) {
    if (s == "LengthField") { out = Core::FramingType::LengthField; return true; }
    if (s == "Fixed")       { out = Core::FramingType::Fixed; return true; }
    if (s == "Silence")     { out = Core::FramingType::Silence; return true; }
    if (s == "Message")     { out = Core::FramingType::Message; return true; }
    return false;
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
        case Core::FramingType::LengthField:
            ok = OptInt(j, "lengthFieldOffset", f.lengthField.lengthFieldOffset, err)
              && OptInt(j, "lengthFieldLength", f.lengthField.lengthFieldLength, err)
              && OptBool(j, "lengthIncludesHeader", f.lengthField.lengthIncludesHeader, err)
              && OptInt(j, "headerLength", f.lengthField.headerLength, err)
              && OptInt(j, "lengthAdjustment", f.lengthField.lengthAdjustment, err)
              && OptInt(j, "maxFrameSize", f.lengthField.maxFrameSize, err);
            if (ok && j.contains("byteOrder")) {
                std::string boStr;
                if (!OptStr(j, "byteOrder", boStr, err) ||
                    !ByteOrderFromString(boStr, f.lengthField.byteOrder)) {
                    if (err.empty()) err = "未知 byteOrder: \"" + boStr + "\"";
                    ok = false;
                }
            }
            break;
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
namespace {
int TemplateElementBytes(const std::string& el) {
    if (!el.empty() && el[0] == '{') {
        std::vector<std::string> parts;
        if (SplitPlaceholder(el, parts) && !parts.empty()) {
            const std::string& fmt = parts[parts.size() - 1];
            if (fmt == "raw")  return 0;
            if (fmt == "X2")   return 1;
            if (fmt == "X4")   return 2;
            if (fmt == "X8")   return 4;
            if (fmt == "X16")  return 8;
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
    if (!SplitPlaceholder(el, parts) || parts.empty()) return false;
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
} // namespace

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
                if (allowed.find(id) == allowed.end()) {
                    err = prefix + ".outputs." + kv.first
                        + ".expr 引用了未声明的输入变量 \"" + id
                        + "\" (仅允许引用同一作用域 inputs 已声明名或模板 {Name:raw} 载荷名, "
                          "{name:len} 取字节长度, {Frame:fixed}/{name:offset} 为模板结构原语)";
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
    }
        // 操作语义标注 (可选; 空 = 未标注, "read" | "write" 由校验器核对)
    if (!OptStr(j, "kind", op.kind, err)) return false;
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

        // 注: 协议级无 simulation 段 (位于 server.json — ServerConfig.simulation)
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
                    || !OptStr(tj, "finalType", tag.finalType, err)
                    || !OptInt(tj, "scanRateMs", tag.scanRateMs, err)
                                        // registerCount 顶层字段不存在; 走 variables.ByteCount (协议族字节单位)
                                        // reportMode / deadband 顶层字段不存在 — 消费侧无落点 (见 ROADMAP 上报过滤)
                                        // 写标签: direction/writeVariable/readBackTag (缺省走 TagDefinition 构造默认)
                    || !OptStr(tj, "direction", tag.direction, err)
                    || !OptStr(tj, "writeVariable", tag.writeVariable, err)
                    || !OptStr(tj, "readBackTag", tag.readBackTag, err)
                                        // 标签级写能力: writeOperation + writeVariables (缺省 = 只读)
                    || !OptStr(tj, "writeOperation", tag.writeOperation, err)
                    || !OptStr(tj, "writeBytesOperation", tag.writeBytesOperation, err)
                                        // 位偏移为标签一等字段 (缺省 -1 = 未声明)
                    || !OptInt(tj, "bitOffset", tag.bitOffset, err)) {
                return false;
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

} // namespace

// ════════════════════════ 类成员实现 ════════════════════════

ConfigValidator::ConfigValidator(int supportedSchemaVersion)
    : _supportedVersion(supportedSchemaVersion) {}

// ── 版本门禁 (§7 前置门禁, 唯一提前终止) ──

bool ConfigValidator::checkVersionGate(int protoVersion, ValidationResult& r) const {
    if (protoVersion == _supportedVersion) return true;
    if (protoVersion < _supportedVersion) {
        r.addError("配置代际过旧: schemaVersion=" + std::to_string(protoVersion)
                   + " < 受支持版本 " + std::to_string(_supportedVersion)
                   + ", 请先迁移配置 (ADR-0005)");
    } else {
        r.addError("配置代际过新: schemaVersion=" + std::to_string(protoVersion)
                   + " > 受支持版本 " + std::to_string(_supportedVersion)
                   + ", 请升级程序 (ADR-0005)");
    }
    return false;
}

// ── 协议层 (规则 1-8) ──

ValidationResult ConfigValidator::validateProtocol(const Core::ProtocolConfig& proto) const {
    ValidationResult r;
    if (!checkVersionGate(proto.schemaVersion, r)) return r;   // 门禁失败不再继续字段校验

    // 规则 1: protocolName 非空 (全局唯一性由 validateConfigRoot 跨协议检查)
    if (proto.protocolName.empty()) r.addError("protocolName 为空 (规则1)");

    validateTransport(proto.transport, r);
    validateFraming(proto.framing, proto.transport, r);

    // 模板文法已收缩为 {Name:Xn} / {Name:raw} 两段:
        //   - builtInFunctions 与三段校验和文法均不支持;
    //   - 校验和改走声明式 inputs.source=auto strategy=crc + params.algo
    //     (crc16-modbus/crc16-ccitt/crc32), 派生长度走 outputs 的 derivedLength;
    //   - :auto:/:calc: 令牌移除后, 自动计算职责由 variables.source=auto 声明承担.

    // 规则 6: operations 非空 + 模板文法
    if (proto.operations.empty()) {
        r.addError("operations 不能为空 (规则6)");
    } else {
        for (std::unordered_map<std::string, Core::OperationConfig>::const_iterator
                it = proto.operations.begin(); it != proto.operations.end(); ++it) {
            const Core::OperationConfig& op = it->second;
            if (op.requestTemplate.empty()) {
                r.addError("操作 " + it->first + " requestTemplate 不能为空 (规则6)");
                continue;
            }
            for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
                validateTemplate(it->first, op.requestTemplate[i], r);
            }
                        // kind 语义标注值域 (空 = 未标注, 允许)
            if (!op.kind.empty() && op.kind != "read" && op.kind != "write") {
                r.addError("操作 " + it->first + " kind 取值非法: \"" + op.kind
                           + "\" (仅 read/write) (规则6)");
            }
                        // 长度偏移自检 (成帧长度槽位派生变量 ↔ 模板固定字节布局)
            ValidateDerivedLengthOffsets(proto, op, it->first, r);
                        // 注: 操作级无 timeoutMs — 超时统一由 device.requestTimeoutMs 配置
        }
    }

    validateHandshake(proto.handshake, proto.transport, r);

        // outputs(derivedLength) expr 整数除法截断警告
    //   除数非 2 的幂时, 整数除法将截断小数部分, 可能导致地址偏移.
    //   例: StartByteAddress / 3 → 地址 5 → 1 (丢失 0.666), 实际访问地址 2.
    //   仅扫描协议级 + 各操作级 outputs 段.
    auto checkDivWarning = [&](const std::string& ctxPrefix,
                               const std::unordered_map<std::string, Core::VariableConfig>& outs) {
        for (auto& kv : outs) {
            const Core::VariableConfig& v = kv.second;
            if (!v.isDerivedLength() || v.expr.empty()) continue;
            const std::string& expr = v.expr;
            const size_t n = expr.size();
            size_t i = 0;
            while (i < n) {
                if (expr[i] == '/') {
                    size_t j = i + 1;
                    while (j < n && (expr[j] == ' ' || expr[j] == '\t')) ++j;
                    if (j >= n) { i = j; continue; }
                    if (std::isdigit(static_cast<unsigned char>(expr[j]))) {
                        size_t k = j;
                        while (k < n && std::isdigit(static_cast<unsigned char>(expr[k]))) ++k;
                        std::string numStr = expr.substr(j, k - j);
                        try {
                            unsigned long long divisor = std::stoull(numStr);
                            if (divisor > 0 && (divisor & (divisor - 1ULL)) != 0ULL) {
                                r.addWarning(ctxPrefix + ".outputs." + kv.first
                                    + ".expr 含除法 /" + numStr
                                    + " (除数非 2 的幂), 整数除法将截断小数部分, "
                                      "可能导致地址偏移; 建议确认地址语义或改用 2 的幂除数 (如 /2 /4 /8 /16)");
                            }
                        } catch (...) { /* 解析失败, 跳过 */ }
                    }
                }
                ++i;
            }
        }
    };
    checkDivWarning(proto.protocolName, proto.outputs);
    for (auto& kv : proto.operations) {
        checkDivWarning(proto.protocolName + ".operations." + kv.first, kv.second.outputs);
    }

        // 读路径 {WriteValue:len} 警告 — 读操作 totalBytes=0, 该属性返回 0
    //   例: 读操作 outputs 中 expr 含 {WriteValue:len} → 运行时恒为 0, 派生值无意义.
    //   写操作不受影响 (WriteBytes 路径 totalBytes 为实际载荷字节数).
    for (auto& kv : proto.operations) {
        if (kv.second.kind != "read") continue;
        for (auto& okv : kv.second.outputs) {
            if (!okv.second.isDerivedLength() || okv.second.expr.empty()) continue;
            const std::string& expr = okv.second.expr;
            if (expr.find("{WriteValue:len}") != std::string::npos) {
                r.addWarning(proto.protocolName + ".operations." + kv.first
                    + ".outputs." + okv.first
                    + ".expr 含 {WriteValue:len}, "
                      "读路径 totalBytes=0 该属性恒返回 0, 派生值无意义; "
                      "若需写载荷长度请在写操作 (kind=write) 的 outputs 中声明");
            }
        }
    }

    // 注: 协议级 simulation 文法校验已删除 (v1.1 simulation 移至 server.json,
    // responseTemplate 文法校验在 ConfigDirectoryLoader::ParseServerJson 完成)
    return r;
}

void ConfigValidator::validateTransport(const Core::TransportConfig& t,
                                        ValidationResult& r) const {
    switch (t.type) {
        case Core::TransportType::Tcp:
            break;
        case Core::TransportType::Tls:
            // 规则 2: Tls caFile 路径存在性仅 Warning
            if (!t.tls.caFile.empty() && !FileExists(t.tls.caFile)) {
                r.addWarning("TLS caFile 文件不存在: " + t.tls.caFile + " (规则2)");
            }
            // 能力前移提示: TLS 运行期通道尚未实现 (tls 暂缓决策), 加载即告警
            r.addWarning("TLS 传输的运行期通道尚未实现, 设备将无法建立连接 "
                         "(当前版本仅支持 Tcp)");
            break;
        case Core::TransportType::Serial:
            if (t.serial.baudRate == 0) r.addError("Serial baudRate 必须 > 0 (规则2)");
            break;
        default:
            r.addError("transport.type 判别值越界 (规则2)");
            break;
    }
}

void ConfigValidator::validateFraming(const Core::FramingConfig& f,
                                      const Core::TransportConfig& t,
                                      ValidationResult& r) const {
    switch (f.type) {
        case Core::FramingType::LengthField: {
            const Core::LengthFieldConfig& lf = f.lengthField;
            // 规则 3
            if (lf.lengthFieldLength != 1 && lf.lengthFieldLength != 2
                    && lf.lengthFieldLength != 4) {
                r.addError("LengthField lengthFieldLength 取值须为 1/2/4, 实际 "
                           + std::to_string(lf.lengthFieldLength) + " (规则3)");
            }
            if (lf.lengthFieldOffset < 0) {
                r.addError("LengthField lengthFieldOffset 必须 ≥ 0 (规则3)");
            }
            if (lf.headerLength != 0
                    && lf.headerLength < lf.lengthFieldOffset + lf.lengthFieldLength) {
                r.addError("headerLength 必须 ≥ lengthFieldOffset + lengthFieldLength, 或为 0 (规则3)");
            }
            const int effHeader = lf.headerLength > 0
                ? lf.headerLength
                : lf.lengthFieldOffset + lf.lengthFieldLength;
            if (lf.maxFrameSize <= effHeader) {
                r.addError("maxFrameSize (" + std::to_string(lf.maxFrameSize)
                           + ") 必须 > 有效帧头长度 (" + std::to_string(effHeader) + ") (规则3)");
            }
            break;
        }
        case Core::FramingType::Fixed:
            // 规则 4
            if (f.fixed.fixedLength <= 0) {
                r.addError("Fixed fixedLength 必须 > 0 (规则4)");
            }
            break;
        case Core::FramingType::Silence:
            // 规则 5
            // v1.27 改: frameGapUs 必须 >= 1 — 引擎已移除 3.5×charTimeUs 默认 (Modbus RTU 约定),
            //   静默阈值须显式配置, 0 表示未配置, 运行期通道将拒发.
            if (f.silence.charTimeUs < 0 || f.silence.frameGapUs <= 0) {
                r.addError("Silence frameGapUs 必须 ≥ 1 (静态静默阈值须显式配置; "
                           "v1.27 起不再默认 3.5×charTimeUs, 请按协议填写帧间静默间隔(us)) (规则5)");
            }
            if (f.silence.maxFrameSize <= 0) {
                r.addError("Silence maxFrameSize 必须 > 0 (规则5)");
            }
            if (f.silence.charTimeUs == 0
                    && t.type == Core::TransportType::Serial
                    && t.serial.baudRate == 0) {
                r.addError("charTimeUs=0 时协议级 baudRate 必须 > 0 以折算字符时间 (规则5)");
            }
            break;
        case Core::FramingType::Message:
            // ADR-0002: CAN 消息成帧预留, v1 报名称错误
            r.addError("framing.type=\"Message\" 为 CAN 预留, v1 不支持 (ADR-0002)");
            break;
        default:
            r.addError("framing.type 判别值越界 (规则2)");
            break;
    }
}

void ConfigValidator::validateHandshake(const std::vector<Core::HandshakeStep>& hs,
                                        const Core::TransportConfig& transport,
                                        ValidationResult& r) const {
    for (size_t i = 0; i < hs.size(); ++i) {
        const Core::HandshakeStep& step = hs[i];
        const std::string ctx = step.name.empty()
            ? ("handshake[" + std::to_string(i) + "]")
            : ("握手步骤 " + step.name);

        if (step.name.empty()) r.addError(ctx + " 缺失步骤名 (规则8)");
        if (step.requestTemplate.empty()) {
            r.addError(ctx + " requestTemplate 不能为空 (规则6)");
        } else {
            for (size_t k = 0; k < step.requestTemplate.size(); ++k) {
                validateTemplate(ctx, step.requestTemplate[k], r);
            }
        }

        // 规则 8: sessionExtractExpr 与 sessionVariable 成对出现
        const bool hasExtract = !step.sessionExtractExpr.empty();
        const bool hasVar = !step.sessionVariable.empty();
        if (hasExtract != hasVar) {
            r.addError(ctx + " sessionExtractExpr 与 sessionVariable 必须成对出现 (规则8)");
        }

        if (step.framingOverride.has_value()) {
            validateFraming(step.framingOverride.value(), transport, r);
        }
        if (step.timeoutMs < 0) {
            r.addError(ctx + " timeoutMs 必须 ≥ 0 (0 = 继承连接超时)");
        }
    }
}

// ── 模板文法 (§3 / 规则 6-7) ──

/// 将一行模板按空格 / 制表符切分成子 token; 纯 hex 行与单占位符行无需切分
/// (分别由 isValidHexLiteral / isValidPlaceholder 直接判过); 仅混合行才进入切分。
/// 返回值: true 表示切分成功 (含 0 token 的"全空白"也视为 true),
///         token 列表按从左到右顺序填入 out。
static bool SplitTemplateTokens(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t') { flush(); continue; }
        cur += c;
    }
    flush();
    return true;
}

// v1.13 长度偏移自检: 见上方 namespace 辅助; 对 protocol.outputs(可被 op 覆盖) 中
//   位于成帧长度槽位的派生变量, 校验其相对 payload 的常量偏移与模板固定字节布局一致.
void ConfigValidator::ValidateDerivedLengthOffsets(
        const Core::ProtocolConfig& proto, const Core::OperationConfig& op,
        const std::string& opName, ValidationResult& r) const {
    const Core::LengthFieldConfig& lf = proto.framing.lengthField;
    if (proto.framing.type != Core::FramingType::LengthField || lf.lengthFieldLength == 0) return;

    struct Slot { std::string name; int off; int width; };
    std::vector<Slot> slots;
    int cursor = 0;
    for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
        const std::string& el = op.requestTemplate[i];
        std::string nm;
        if (TemplateElementName(el, nm)) {
            Slot s; s.name = nm; s.off = cursor; s.width = TemplateElementBytes(el);
            slots.push_back(s);
        }
        cursor += TemplateElementBytes(el);
    }
    const int total = cursor;

    // raw 变长载荷占位 (该操作无 raw → 不涉及帧长派生, 跳过)
    // v1.22: 载荷名 = 模板 {Name:raw} 占位符名 (无需 inputs 特殊声明)
    const std::set<std::string> rawNames = CollectRawPlaceholderNames(op.requestTemplate);
    std::string rawName = rawNames.empty() ? std::string() : *rawNames.begin();
    int Ppayload = -1;
    for (size_t i = 0; i < slots.size() && Ppayload < 0; ++i) {
        if (slots[i].name == rawName && slots[i].width == 0) Ppayload = slots[i].off;
    }
    if (Ppayload < 0) return;
    const long afterPayload = total - Ppayload;

    for (size_t i = 0; i < slots.size(); ++i) {
        const Slot& sl = slots[i];
        if (sl.off != lf.lengthFieldOffset || sl.width != lf.lengthFieldLength) continue;
        // 槽位变量: op 级 outputs 覆盖优先, 否则协议级 outputs (v1.17 方案B: 派生输出在 outputs 段)
        const Core::VariableConfig* v = 0;
        std::unordered_map<std::string, Core::VariableConfig>::const_iterator vit =
            op.outputs.find(sl.name);
        if (vit != op.outputs.end()) { v = &vit->second; }
        else {
            std::unordered_map<std::string, Core::VariableConfig>::const_iterator pit =
                proto.outputs.find(sl.name);
            if (pit != proto.outputs.end()) v = &pit->second;
        }
        if (!v || !v->isDerivedLength()) continue;
        long K = 0; bool known = false;
        if (!v->expr.empty()) {                       // v1.16: 仅 expr (kind 已移除)
            if (TryPayloadOffset(v->expr, rawName, K)) known = true;
        }
        if (!known) continue;
        const long expected = lf.lengthIncludesHeader
            ? (Ppayload + afterPayload)
            : (Ppayload - (static_cast<long>(sl.off) + sl.width) + afterPayload);
        if (K != expected) {
            r.addError("操作 " + opName + " 派生长度 \"" + sl.name + "\" 偏移自检失败: "
                "声明偏移 " + std::to_string(K) + " ≠ 模板期望 " + std::to_string(expected)
                + " (成帧长度槽位 offset=" + std::to_string(lf.lengthFieldOffset)
                + " length=" + std::to_string(lf.lengthFieldLength)
                + " includesHeader=" + (lf.lengthIncludesHeader ? "true" : "false")
                + "; 改动模板固定字节后需同步该派生长度偏移)");
        }
    }
}

/// 已移除的三段文法 {Name:seg2:Xn} → 定向报错并指引迁移路径 (否则会退化成
/// 泛化的"既非十六进制字面量也非合法占位符", 用户无从知道该改什么)
static bool RejectRemovedThreeSegmentSyntax(const std::string& opName,
                                            const std::string& line,
                                            const std::string& tok,
                                            ValidationResult& r) {
    std::vector<std::string> parts;
    if (!SplitPlaceholder(tok, parts) || parts.size() != 3) return false;
    r.addError("操作 " + opName + " 模板占位符 \"" + tok + "\" 为已移除的三段文法; "
               "模板仅支持 {Name:Xn} 与 {Name:raw} — 校验和改用声明式 "
               "inputs.source=auto strategy=crc (+params.algo: "
               "crc16-modbus/crc16-ccitt/crc32), 派生长度改用 outputs 的 derivedLength"
               " (规则6, 原行: \"" + line + "\")");
    return true;
}

void ConfigValidator::validateTemplate(const std::string& opName, const std::string& line,
                                       ValidationResult& r) const {
    if (line.empty()) {
        r.addError("操作 " + opName + " 存在空模板行 (规则6)");
        return;
    }
    if (line.find(":auto:") != std::string::npos) {
        r.addError("操作 " + opName + " 模板含已移除的 :auto: 令牌 (v1.16); 改写作 \"{Name:Xn}\" 并在 variables 声明 source=auto");
        return;
    }
    if (isValidHexLiteral(line)) return;                       // §3.1 十六进制字面量 (允许多 token)
    if (isValidPlaceholder(line)) return;                      // §3.2 单占位符 {Name:Xn} / {Name:raw}
    if (RejectRemovedThreeSegmentSyntax(opName, line, line, r)) return;

    // §3.3 混合行: 按空格切分成多个 token, 每个 token 单独判定
    std::vector<std::string> tokens;
    SplitTemplateTokens(line, tokens);
    if (tokens.empty()) {
        r.addError("操作 " + opName + " 模板行全为空白 (规则6)");
        return;
    }

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& tok = tokens[i];
        if (isValidHexLiteral(tok)) continue;
        if (!isValidPlaceholder(tok)) {
            if (RejectRemovedThreeSegmentSyntax(opName, line, tok, r)) return;
            r.addError("操作 " + opName + " 模板 token 既非十六进制字面量也非合法占位符: \""
                       + tok + "\" (规则6, 原行: \"" + line + "\")");
            return;
        }
    }
}

// ── 设备层 (规则 9-11) ──

void ConfigValidator::validateDevice(const Core::DeviceConfig& dev,
                                     const std::unordered_map<std::string,
                                         const Core::ProtocolConfig*>& protoMap,
                                     ValidationResult& r) const {
    const std::string ctx = dev.id.empty() ? std::string("(匿名设备)") : dev.id;

    if (dev.connection.timeoutMs <= 0) {
        r.addError("设备 " + ctx
                   + " connection.timeoutMs 必须 > 0 "
                   + "(0/缺省 = asio 立即过期, 表现为连接超时, 不允许)");
    }
    if (dev.requestTimeoutMs <= 0) {
        r.addError("设备 " + ctx
                   + " requestTimeoutMs 必须 > 0 "
                   + "(0/缺省 = 单次请求-应答立即超时, 不允许)");
    }

    std::unordered_map<std::string, const Core::ProtocolConfig*>::const_iterator pit =
        protoMap.find(dev.protocol);
    if (pit == protoMap.end()) {
        // 规则 9: protocol 引用存在的协议 (跨文件)
        r.addError("设备 " + ctx + " 引用不存在的协议: \"" + dev.protocol + "\" (规则9)");
        return;
    }
    const Core::ProtocolConfig* proto = pit->second;

    switch (proto->transport.type) {
        case Core::TransportType::Tcp:
        case Core::TransportType::Tls: {
            // 规则 10
            if (dev.connection.host.empty()) {
                r.addError("Tcp/Tls 设备 " + ctx + " connection.host 不能为空 (规则10)");
            }
            if (dev.connection.port == 0) {
                const uint16_t def = proto->transport.type == Core::TransportType::Tcp
                    ? proto->transport.tcp.defaultPort
                    : proto->transport.tls.defaultPort;
                if (def == 0) {
                    r.addError("设备 " + ctx + " port=0 且协议 defaultPort=0, 无可用端口 (规则10)");
                }
            }
            break;
        }
        case Core::TransportType::Serial:
            // 规则 11: 设备级或协议级 portName 至少一个非空
            if (dev.connection.portName.empty()
                    && proto->transport.serial.portName.empty()) {
                r.addError("Serial 设备 " + ctx
                           + " 与其协议均未指定 portName (规则11)");
            }
            break;
        default:
            break;
    }

    if (dev.resilience.has_value()) {
        validateResilience(dev.resilience.value(), "设备 " + ctx, r);
    }
}

// ── 标签层 (规则 12-15) ──

void ConfigValidator::validateTag(const Core::TagDefinition& tag,
                                  const std::unordered_map<std::string,
                                      const Core::DeviceConfig*>& deviceMap,
                                  const std::unordered_map<std::string,
                                      const Core::ProtocolConfig*>& protoMap,
                                  ValidationResult& r) const {
    const std::string ctx = tag.name.empty() ? std::string("(匿名标签)") : tag.name;

    // 规则 13
    // v1.6 写标签 (direction=write): 不参与轮询, scanRateMs 不作要求;
    //       readBackTag 仅写标签可配; writeVariable 不可为空.
    const bool isWriteTag = (tag.direction == "write");
    if (tag.direction != "read" && !isWriteTag) {
        r.addError("标签 " + ctx + " direction 取值非法: \"" + tag.direction
                   + "\" (仅 read/write) (规则13)");
    }
    if (!isWriteTag && tag.scanRateMs <= 0) {
        r.addError("标签 " + ctx + " scanRateMs 必须 > 0 (规则13)");
    }
    if (isWriteTag && tag.writeVariable.empty()) {
        r.addError("标签 " + ctx + " 写标签 writeVariable 不能为空 (规则13)");
    }
    if (!isWriteTag && !tag.readBackTag.empty()) {
        r.addError("标签 " + ctx + " readBackTag 仅写标签 (direction=write) 可配置 (规则13)");
    }
    // v1.7/v1.32 标签级写能力: writeOperation/writeBytesOperation/writeVariables
    //      仅 direction=read 标签有意义; 非空 = 读写标签, 全空 = 只读 (写 API 明确拒绝).
    if (isWriteTag && !tag.writeOperation.empty()) {
        r.addWarning("标签 " + ctx + " 写标签 (direction=write) 无需 writeOperation, 将被忽略 (规则13)");
    }
    if (isWriteTag && !tag.writeBytesOperation.empty()) {
        r.addWarning("标签 " + ctx + " 写标签 (direction=write) 无需 writeBytesOperation, 将被忽略 (规则13)");
    }
    if (isWriteTag && !tag.writeVariables.empty()) {
        r.addWarning("标签 " + ctx + " 写标签 (direction=write) 无需 writeVariables, 将被忽略 (规则13)");
    }
    if (!isAllowedFinalType(tag.finalType)) {
        r.addError("标签 " + ctx + " finalType 取值非法: \"" + tag.finalType
                   + "\" (规则13, 见 Config_Schema §6)");
    }

    // v1.26 位粒度: BitOffset 约定键校验 (语义 = 字节内位偏移 0-7).
    //   Error: 超范围 (运行时 ResponseParser 亦有防御);
    //   Warning: 声明 BitOffset 但 finalType 非 Bool (当前无消费方);
    //            Bool 未声明 BitOffset (位寻址 op 需显式声明, 否则解析走
    //            raw[0]!=0 旧语义, 线圈场景会误报同字节其他位);
    //            Bool + BitOffset + ByteCount > 1 (合法但疑似笔误, 位标签跨度应为 1).
    {
        // v1.28: 位偏移改读标签一等字段 tag.bitOffset (-1 = 未声明), 不再查 variables 魔法键
        const bool hasBit = (tag.bitOffset >= 0);
        const bool isBool = (tag.finalType == "Bool");
        if (hasBit && tag.bitOffset > 7) {
            r.addError("标签 " + ctx + " bitOffset 必须 0-7, 实际 "
                       + std::to_string(tag.bitOffset) + " (规则13, v1.28)");
        }
        if (hasBit && !isBool) {
            r.addWarning("标签 " + ctx + " 声明了 bitOffset 但 finalType=\""
                         + tag.finalType + "\" 非 Bool, 位偏移无消费方 (规则15, v1.28)");
        }
        if (isBool && !hasBit) {
            r.addWarning("标签 " + ctx + " finalType=Bool 未声明 bitOffset, "
                         "解析将按整字节 raw[0]!=0 (v1.25 旧语义); 线圈/离散量位寻址须显式声明 (规则15, v1.28)");
        }
        if (isBool && hasBit) {
            std::unordered_map<std::string, uint32_t>::const_iterator bc =
                tag.variables.find(Core::ByteCountVariableName());
            if (bc != tag.variables.end() && bc->second > 1u) {
                r.addWarning("标签 " + ctx + " Bool 位标签 ByteCount=" + std::to_string(bc->second)
                             + " > 1, 位提取仅取首字节, 跨度将被合并请求放大 (疑似笔误) (规则15, v1.26)");
            }
        }
    }

    // 规则 15 Warning — v1.25 改: 跨协议字节单位 "ByteCount" (约定键).
    //   与 tags.json 中读取标签声明的 {ByteCount:N} 与协议 JSON 协议族单位 {RegisterCount:M} (Modbus)
//     一致性由协议 JSON derivedLength 自身保证, 此处不再做跨字段核对 (避免把协议族知识渗入校验).
    //   仍保留 {StartByteAddress} > 0 的合理性警告 (按字节地址线性推断).
    // 引用链: tag → device → protocol → operation
    std::unordered_map<std::string, const Core::DeviceConfig*>::const_iterator dit =
        deviceMap.find(tag.deviceId);
    if (dit == deviceMap.end()) {
        r.addError("标签 " + ctx + " 引用不存在的设备: \"" + tag.deviceId + "\" (规则12)");
        return;
    }
    const Core::DeviceConfig* dev = dit->second;

    std::unordered_map<std::string, const Core::ProtocolConfig*>::const_iterator pit =
        protoMap.find(dev->protocol);
    if (pit == protoMap.end()) {
        r.addError("标签 " + ctx + " 所属设备的协议不存在: \"" + dev->protocol + "\" (规则12)");
        return;
    }
    const Core::ProtocolConfig* proto = pit->second;

    // 规则 14 变量可解析域: 握手 sessionVariable 声明集
    std::set<std::string> sessionVars;
    for (size_t i = 0; i < proto->handshake.size(); ++i) {
        if (!proto->handshake[i].sessionVariable.empty()) {
            sessionVars.insert(proto->handshake[i].sessionVariable);
        }
    }

    // 规则 12: operation 引用存在; 规则 14: 占位符变量可解析
    std::unordered_map<std::string, Core::OperationConfig>::const_iterator oit =
        proto->operations.find(tag.operation);
    if (oit == proto->operations.end()) {
        r.addError("标签 " + ctx + " 操作引用不存在: \"" + tag.operation
                   + "\" (协议 " + dev->protocol + ") (规则12)");
        return;
    }
    // v1.7: 操作 kind 语义一致性 (kind 未标注 = 跳过检查)
    //   读标签 operation 应为 read 类; 写标签 operation 应为 write 类;
    //   读写标签的 writeOperation 应为 write 类 (下方校验).
    if (!oit->second.kind.empty()) {
        const bool opIsRead = (oit->second.kind == "read");
        const bool opIsWrite = (oit->second.kind == "write");
        if (isWriteTag && !opIsWrite) {
            r.addError("标签 " + ctx + " 写标签 operation 指向非写操作: "
                       + tag.operation + " (kind=" + oit->second.kind + ") (规则12)");
        } else if (!isWriteTag && !opIsRead) {
            r.addError("标签 " + ctx + " operation 指向非读操作: "
                       + tag.operation + " (kind=" + oit->second.kind + ") (规则12)");
        }
    }
    // v1.7: writeOperation 引用存在 + kind 一致性 (规则12)
    if (!isWriteTag && !tag.writeOperation.empty()) {
        std::unordered_map<std::string, Core::OperationConfig>::const_iterator wIt =
            proto->operations.find(tag.writeOperation);
        if (wIt == proto->operations.end()) {
            r.addError("标签 " + ctx + " 写操作引用不存在: \"" + tag.writeOperation
                       + "\" (协议 " + dev->protocol + ") (规则12)");
        } else if (!wIt->second.kind.empty() && wIt->second.kind != "write") {
            r.addError("标签 " + ctx + " writeOperation 指向非写操作: "
                       + tag.writeOperation + " (kind=" + wIt->second.kind + ") (规则12)");
        }
    }
    // v1.32: writeBytesOperation 引用存在 + kind 一致性 (规则12)
    if (!isWriteTag && !tag.writeBytesOperation.empty()) {
        std::unordered_map<std::string, Core::OperationConfig>::const_iterator wbIt =
            proto->operations.find(tag.writeBytesOperation);
        if (wbIt == proto->operations.end()) {
            r.addError("标签 " + ctx + " 变长写操作引用不存在: \"" + tag.writeBytesOperation
                       + "\" (协议 " + dev->protocol + ") (规则12)");
        } else if (!wbIt->second.kind.empty() && wbIt->second.kind != "write") {
            r.addError("标签 " + ctx + " writeBytesOperation 指向非写操作: "
                       + tag.writeBytesOperation + " (kind=" + wbIt->second.kind + ") (规则12)");
        }
    }

    const std::vector<std::string>& tpl = oit->second.requestTemplate;
    // v1.6/v1.7: 运行时注入的变量加入解析域白名单 —
    //   writeVariable (数值/字节注入) + 起始地址(固定 "StartAddress", v1.19 移除 addressVariable 配置)
    //   + WriteBytes 派生长度族 (PDULength/DataLength/DataBits/DataLen/RegisterCount/ByteCount)
    //   适用对象: 写标签 (direction=write) 与声明了写能力的读写标签
    //   (writeOperation / writeBytesOperation 任一非空)
    // v1.25: 读标签同样放行 outputs(derivedLength) 派生名 — 协议族变量
    //   (如 Modbus StartAddress/RegisterCount) 现由协议 JSON outputs 派生供给,
    //   tag.variables 只声明跨协议字节单位 (StartByteAddress/ByteCount).
    std::set<std::string> runtimeVars;
    {
        const Core::ProtocolConfig& pr = *proto;
        for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                pr.outputs.begin(); it != pr.outputs.end(); ++it)
            if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
        for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                oit->second.outputs.begin(); it != oit->second.outputs.end(); ++it)
            if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
    }
    if (isWriteTag || !tag.writeOperation.empty()
            || !tag.writeBytesOperation.empty()) {
        if (!tag.writeVariable.empty()) runtimeVars.insert(tag.writeVariable);
        // v1.28 删: 原此处无条件插入 "StartAddress" 已移除 —
        //   协议族地址名 (StartAddress/RegisterCount/...) 一律由上方 protocol.outputs /
        //   op.outputs 的 derivedLength 声明动态收集, 不再假定任何具体名 (契约名收敛为 2 个).
        // v1.11: 派生长度改由 JSON 声明 (source=auto strategy=derivedLength) — 收集声明名放行.
        //   v1.12: 旧 PDULengthRegistry 兜底已移除; PDULength 仅当其声明为 derivedLength 才放行.
        //   v1.17 方案B: derivedLength 只存在于 outputs 段 (inputs 不允许 derivedLength 已校验).
        if (!isWriteTag && !tag.writeOperation.empty()) {
            std::unordered_map<std::string, Core::OperationConfig>::const_iterator wit2 =
                proto->operations.find(tag.writeOperation);
            if (wit2 != proto->operations.end())
                for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                        wit2->second.outputs.begin(); it != wit2->second.outputs.end(); ++it)
                    if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
        }
        // v1.32: 变长写模板的派生名同样放行 (如 RegisterCount = {WriteValue:len} / 2)
        if (!isWriteTag && !tag.writeBytesOperation.empty()) {
            std::unordered_map<std::string, Core::OperationConfig>::const_iterator wbit2 =
                proto->operations.find(tag.writeBytesOperation);
            if (wbit2 != proto->operations.end())
                for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                        wbit2->second.outputs.begin(); it != wbit2->second.outputs.end(); ++it)
                    if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
        }
    }
    // v1.27 增: 标签级 variables 若声明了 outputs 派生名, 警告将覆盖派生值
    //   例: 标签声明 StartAddress=10, 但协议 outputs.StartAddress = StartByteAddress/2 派生.
    //   标签显式值覆盖派生值, 用户可能不理解地址来源.
    for (std::unordered_map<std::string, uint32_t>::const_iterator it =
            tag.variables.begin(); it != tag.variables.end(); ++it) {
        if (runtimeVars.find(it->first) != runtimeVars.end()) {
            r.addWarning("标签 " + ctx + " variables 声明了 outputs 派生名 \""
                         + it->first + "=" + std::to_string(it->second)
                         + " (协议 outputs 将按 derivedLength 表达式派生), "
                           "标签显式值将覆盖派生值; 若为有意覆盖可忽略");
        }
    }
    // 规则 14 占位符校验 (读模板用 tag.variables; 写模板用 variables ∪ writeVariables)
    auto checkPlaceholders =
            [&](const std::vector<std::string>& t,
                const std::unordered_map<std::string, uint32_t>& vars) {
        for (size_t i = 0; i < t.size(); ++i) {
            if (!isValidPlaceholder(t[i])) continue;             // 文法错误已由协议层上报

            std::vector<std::string> parts;
            if (!SplitPlaceholder(t[i], parts)) continue;
            const std::string& varName = parts[0];
            const bool isRaw = (parts.size() == 2 && IsRawSpec(parts[1]));
            if (isRaw) {
                // {Name:raw} 变长载荷: 唯一合法来源是运行时注入的写值变量
                //   (WriteViaGateway 把 payload 以 writeVariable 为键注入 rawVars;
                //   v1.32 删: tag/device 级 variableBytesHex — 声明值从不进入帧)
                if (runtimeVars.find(varName) == runtimeVars.end()) {
                    r.addError("标签 " + ctx + " 变长模板变量未定义: " + varName
                               + " (须为写值变量 writeVariable, 默认 "
                               + Core::kDefaultWriteValueVariable + ") (规则14)");
                }
                continue;
            }
            if (vars.find(varName) == vars.end()
                    && sessionVars.find(varName) == sessionVars.end()
                    && !ProtocolResolvesInputVariable(*proto, varName)
                    && runtimeVars.find(varName) == runtimeVars.end()) {
                r.addError("标签 " + ctx + " 模板变量未定义: " + varName
                           + " (须出现于 tag.variables / tag.writeVariables / "
                           "protocol.inputs(static/auto) / 握手会话变量声明) (规则14)");
            }
        }
    };
    checkPlaceholders(tpl, tag.variables);
    // v1.7/v1.32: 写模板占位符校验 — 变量域 = variables ∪ writeVariables (同名键覆盖);
    //   标量写 (writeOperation) 与变长写 (writeBytesOperation) 模板均纳入.
    if (!isWriteTag) {
        for (int wsel = 0; wsel < 2; ++wsel) {
            const std::string& wOpName =
                (wsel == 0) ? tag.writeOperation : tag.writeBytesOperation;
            if (wOpName.empty()) continue;
            std::unordered_map<std::string, Core::OperationConfig>::const_iterator wIt =
                proto->operations.find(wOpName);
            if (wIt != proto->operations.end()) {
                std::unordered_map<std::string, uint32_t> writeVars = tag.variables;
                for (std::unordered_map<std::string, uint32_t>::const_iterator iv =
                        tag.writeVariables.begin(); iv != tag.writeVariables.end(); ++iv) {
                    writeVars[iv->first] = iv->second;
                }
                checkPlaceholders(wIt->second.requestTemplate, writeVars);
            }
        }
    }
}

// ── 配置根 (含跨文件引用) ──

ValidationResult ConfigValidator::validateConfigRoot(
    const Core::ConfigRoot& root,
    const std::vector<Core::ProtocolConfig>& protocols) const {

    ValidationResult r;
    if (!checkVersionGate(root.schemaVersion, r)) return r;

    // 协议表构建 + 规则 1 全局唯一
    std::unordered_map<std::string, const Core::ProtocolConfig*> protoMap;
    for (size_t i = 0; i < protocols.size(); ++i) {
        const Core::ProtocolConfig& p = protocols[i];
        if (p.protocolName.empty()) continue;                  // 空名已由 validateProtocol 上报
        if (!protoMap.insert(std::make_pair(p.protocolName, &p)).second) {
            r.addError("protocolName 全局重复: " + p.protocolName + " (规则1)");
        }
    }

    if (root.resilience.has_value()) {
        validateResilience(root.resilience.value(), "全局", r);
    }
    if (root.webApi.has_value()) {
        validateWebApi(root.webApi.value(), r);
    }

    // 设备表构建 + 规则 9 id 唯一
    std::unordered_map<std::string, const Core::DeviceConfig*> deviceMap;
    {
        std::set<std::string> ids;
        for (size_t i = 0; i < root.devices.size(); ++i) {
            const Core::DeviceConfig& d = root.devices[i];
            if (d.id.empty()) { r.addError("设备 id 为空 (规则9)"); continue; }
            if (!ids.insert(d.id).second) {
                r.addError("设备 id 全局重复: " + d.id + " (规则9)");
                continue;
            }
            deviceMap.insert(std::make_pair(d.id, &d));
        }
    }
    for (size_t i = 0; i < root.devices.size(); ++i) {
        validateDevice(root.devices[i], protoMap, r);
    }

    // 标签层: 规则 12 name 唯一 + 引用链
    std::map<std::string, const Core::TagDefinition*> tagMap;
    {
        std::set<std::string> names;
        for (size_t i = 0; i < root.tags.size(); ++i) {
            const Core::TagDefinition& tg = root.tags[i];
            if (tg.name.empty()) { r.addError("标签 name 为空 (规则12)"); continue; }
            if (!names.insert(tg.name).second) {
                r.addError("标签 name 全局重复: " + tg.name + " (规则12)");
            }
            tagMap.insert(std::make_pair(tg.name, &tg));
            validateTag(tg, deviceMap, protoMap, r);
        }
    }
    // v1.6 写标签 readBackTag 引用校验: 须指向存在的读标签
    for (size_t i = 0; i < root.tags.size(); ++i) {
        const Core::TagDefinition& tg = root.tags[i];
        if (tg.direction != "write" || tg.readBackTag.empty()) continue;
        const std::string ctx =
            tg.name.empty() ? std::string("(匿名标签)") : tg.name;
        std::map<std::string, const Core::TagDefinition*>::const_iterator rit =
            tagMap.find(tg.readBackTag);
        if (rit == tagMap.end()) {
            r.addError("写标签 " + ctx + " readBackTag 引用不存在的标签: \""
                       + tg.readBackTag + "\" (规则12)");
        } else if (rit->second->direction == "write") {
            r.addError("写标签 " + ctx + " readBackTag 须引用读标签: \""
                       + tg.readBackTag + "\" 自身是写标签 (规则12)");
        }
    }
    return r;
}

// ── 韧性策略 (§11) ──

void ConfigValidator::validateResilience(const Core::ResilienceConfig& res,
                                         const std::string& ctx,
                                         ValidationResult& r) const {
    const std::string p = "[" + ctx + " resilience] ";
    if (res.maxAttempts < 1)       r.addError(p + "maxAttempts 必须 ≥ 1");
    if (res.backoffBaseMs < 0)     r.addError(p + "backoffBaseMs 必须 ≥ 0");
    if (res.backoffMaxMs < res.backoffBaseMs) {
        r.addError(p + "backoffMaxMs 必须 ≥ backoffBaseMs");
    }
    if (res.failureThreshold < 1)  r.addError(p + "failureThreshold 必须 ≥ 1");
    if (res.cooldownMs < 0)        r.addError(p + "cooldownMs 必须 ≥ 0");
    if (res.halfOpenProbes < 1)    r.addError(p + "halfOpenProbes 必须 ≥ 1");
}

// ── 管理面 (§12) ──

void ConfigValidator::validateWebApi(const Core::WebApiConfig& api, ValidationResult& r) const {
    if (!isValidIPAddress(api.bindAddress)) {
        r.addError("[webApi] bindAddress 不是合法 IPv4 地址: \"" + api.bindAddress + "\"");
    }
    if (api.certFile.empty() != api.keyFile.empty()) {
        r.addError("[webApi] certFile 与 keyFile 须同时为空或同时非空 (仅配其一)");
    }
    // ADR-0012 附录 A.1: WebApiServer 为自研明文 HTTP, TLS 字段当前不生效
    if (!api.certFile.empty()) {
        r.addWarning("[webApi] certFile/keyFile 的管理面 TLS 尚未实现, 当前为明文 HTTP "
                     "(字段已解析但不生效) (ADR-0012 附录A.1)");
    }
    if (api.rateLimitRps < 1) {
        r.addError("[webApi] rateLimitRps 必须 ≥ 1");
    }
    if (api.rateLimitBurst < api.rateLimitRps) {
        r.addError("[webApi] rateLimitBurst 必须 ≥ rateLimitRps");
    }
}

// ── 工具方法 ──

bool ConfigValidator::isValidHexLiteral(const std::string& s) {
    size_t i = 0;
    int tokenLen = 0;
    int tokens = 0;
    for (; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ' ' || s[i] == '\t') {
            if (tokenLen > 0) {
                if (tokenLen != 2) return false;               // 每字节恰好两位十六进制
                ++tokens;
                tokenLen = 0;
            }
            continue;
        }
        if (!IsHexChar(s[i])) return false;
        ++tokenLen;
    }
    return tokens > 0;
}

bool ConfigValidator::isValidPlaceholder(const std::string& s) {
    std::vector<std::string> parts;
    if (!SplitPlaceholder(s, parts)) return false;
    // {Name:Xn} | {Name:raw} (P1 A 变长字节注入); 其余段数一律非法
    // (三段校验和文法已移除, 定向报错见 validateTemplate)
    if (parts.size() != 2) return false;
    return IsFormatSpec(parts[1]) || IsRawSpec(parts[1]);
}

bool ConfigValidator::isValidIPAddress(const std::string& s) {
    if (s.empty()) return false;
    int octets = 0;
    size_t i = 0;
    while (i < s.size()) {
        std::string num;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { num += s[i]; ++i; }
        if (num.empty() || num.size() > 3) return false;
        // 前导零拒绝 (简化判定); 数值 0~255
        int v = 0;
        for (size_t k = 0; k < num.size(); ++k) v = v * 10 + (num[k] - '0');
        if (v > 255) return false;
        if (num.size() > 1 && num[0] == '0') return false;
        ++octets;
        if (i < s.size()) {
            if (s[i] != '.') return false;
            ++i;
            if (i == s.size()) return false;                   // 以 '.' 结尾
        }
    }
    return octets == 4;
}

bool ConfigValidator::isNumericFinalType(const std::string& ft) {
    return ft == "UInt16" || ft == "UInt32" || ft == "UInt64"
        || ft == "Int16" || ft == "Int32" || ft == "Int64"
        || ft == "Float" || ft == "Double";
}

bool ConfigValidator::isAllowedFinalType(const std::string& ft) {
    return isNumericFinalType(ft)
        || ft == "ByteArray" || ft == "Bool" || ft == "String";
}

// ── JSON 文本入口 (供 ConfigStore 直接调用) ──
// 语义对齐 §7 前置门禁: schemaVersion 缺省 → Warning + 假定当前代际;
// 存在但不匹配 → 门禁错误并提前终止 (不再继续字段校验)。

Core::Expected<ProtocolValidationOutcome> ConfigValidator::validateAndParseProtocolJson(
    const std::string& json, int supportedSchemaVersion) {

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                std::string("JSON 语法错误: ") + e.what());
    }
    if (!doc.is_object()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "协议配置根必须是 JSON 对象");
    }

    // 缺省 → Warning + 假定当前代际; 存在但类型非法 → 结构错误
    bool hadVersion = doc.contains("schemaVersion");
    int declaredVer = supportedSchemaVersion;
    if (hadVersion && !doc.at("schemaVersion").is_number_integer()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "schemaVersion 类型非法 (须为整数)");
    }
    if (hadVersion) declaredVer = doc.at("schemaVersion").get<int>();

    ProtocolValidationOutcome out;
    std::string err;
    if (!ParseProtocolDocImpl(doc, supportedSchemaVersion, out.proto, err)) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "协议配置结构错误: " + err);
    }

    ConfigValidator inst(supportedSchemaVersion);
    if (!hadVersion) {
        out.result.addWarning("缺失 schemaVersion, 假定当前代际 "
                              + std::to_string(supportedSchemaVersion));
    }
    out.result.merge(inst.validateProtocol(out.proto));
    return Core::Expected<ProtocolValidationOutcome>(out);
}

Core::Expected<ValidationResult> ConfigValidator::validateProtocolJson(
    const std::string& json, int supportedSchemaVersion) {

    Core::Expected<ProtocolValidationOutcome> r =
        validateAndParseProtocolJson(json, supportedSchemaVersion);
    if (!r.has_value()) return Core::UnexpectedType{r.error()};
    return Core::Expected<ValidationResult>(r.value().result);
}

// 加载期帧一致性试算 (ADR-0012 §2) — 与保存期 ConfigStore::Validate 同一实现.
//   夹带 {Name:raw} 载荷的写操作若长度槽位 / 派生长度与 framing 不自洽, 加载即拒;
//   手工编辑或外部生成的配置不再绕过门禁.
static void MergeFrameConsistencyErrors(
    const std::vector<Core::ProtocolConfig>& protos, ValidationResult& r) {
    for (size_t i = 0; i < protos.size(); ++i) {
        const std::vector<std::string> ferr =
            ConfigValidator::CheckFrameConsistency(protos[i]);
        for (size_t k = 0; k < ferr.size(); ++k) r.addError(ferr[k]);
    }
}

Core::Expected<ValidationResult> ConfigValidator::validateConfigRootJson(
    const std::string& json,
    const std::vector<std::string>& protocolJsons,
    int supportedSchemaVersion) {

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                std::string("JSON 语法错误: ") + e.what());
    }
    if (!doc.is_object()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根必须是 JSON 对象");
    }

    bool hadVersion = doc.contains("schemaVersion");
    if (hadVersion && !doc.at("schemaVersion").is_number_integer()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "schemaVersion 类型非法 (须为整数)");
    }

    Core::ConfigRoot root;
    std::string err;
    if (!ParseRootDocImpl(doc, supportedSchemaVersion, root, err)) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根结构错误: " + err);
    }

    ConfigValidator inst(supportedSchemaVersion);
    ValidationResult vr;
    if (!hadVersion) {
        vr.addWarning("缺失 schemaVersion, 假定当前代际 "
                      + std::to_string(supportedSchemaVersion));
    }

    // 跨文件协议引用: 逐个解析协议 JSON 并做全量校验后合并
    std::vector<Core::ProtocolConfig> protos;
    protos.reserve(protocolJsons.size());
    for (size_t i = 0; i < protocolJsons.size(); ++i) {
        nlohmann::json pdoc;
        try {
            pdoc = nlohmann::json::parse(protocolJsons[i]);
        } catch (const nlohmann::json::parse_error& e) {
            return Core::Unexpected(Core::Error::Code::ParseError,
                                    "协议文件[" + std::to_string(i) + "] JSON 语法错误: "
                                    + e.what());
        }
        Core::ProtocolConfig proto;
        if (!pdoc.is_object()
                || !ParseProtocolDocImpl(pdoc, supportedSchemaVersion, proto, err)) {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "协议文件[" + std::to_string(i) + "] 结构错误: " + err);
        }
        protos.push_back(proto);
    }

    // 全量语义: 先各协议内部规则, 再根 + 跨文件引用
    for (size_t i = 0; i < protos.size(); ++i) {
        vr.merge(inst.validateProtocol(protos[i]));
    }
    MergeFrameConsistencyErrors(protos, vr);
    vr.merge(inst.validateConfigRoot(root, protos));
    return vr;
}

// ── JSON → POCO 解析 (文本入口, 委托匿名 ns 的文档级实现) ──

Core::Expected<Core::ProtocolConfig> ConfigValidator::parseProtocolJson(
    const std::string& json) {

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                std::string("JSON 语法错误: ") + e.what());
    }
    Core::ProtocolConfig proto;
    std::string err;
    if (!doc.is_object()
            || !ParseProtocolDocImpl(doc, Core::kSupportedSchemaVersion, proto, err)) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "协议配置结构错误: " + err);
    }
    return Core::Expected<Core::ProtocolConfig>(proto);
}

Core::Expected<Core::ConfigRoot> ConfigValidator::parseConfigRootJson(
    const std::string& json) {

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                std::string("JSON 语法错误: ") + e.what());
    }
    Core::ConfigRoot root;
    std::string err;
    if (!doc.is_object() || !ParseRootDocImpl(doc, 1, root, err)) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根结构错误: " + err);
    }
    return Core::Expected<Core::ConfigRoot>(root);
}

// ── 校验 + 解析一体: 配置根 (协议集已各自通过校验, 不再重复逐协议解析/校验) ──

Core::Expected<ConfigRootValidationOutcome> ConfigValidator::validateAndParseConfigRootJson(
    const std::string& json,
    const std::vector<Core::ProtocolConfig>& protocols,
    int supportedSchemaVersion) {

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::parse_error& e) {
        return Core::Unexpected(Core::Error::Code::ParseError,
                                std::string("JSON 语法错误: ") + e.what());
    }
    if (!doc.is_object()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根必须是 JSON 对象");
    }

    bool hadVersion = doc.contains("schemaVersion");
    if (hadVersion && !doc.at("schemaVersion").is_number_integer()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "schemaVersion 类型非法 (须为整数)");
    }

    ConfigRootValidationOutcome out;
    std::string err;
    if (!ParseRootDocImpl(doc, supportedSchemaVersion, out.root, err)) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根结构错误: " + err);
    }

    ConfigValidator inst(supportedSchemaVersion);
    if (!hadVersion) {
        out.result.addWarning("缺失 schemaVersion, 假定当前代际 "
                              + std::to_string(supportedSchemaVersion));
    }
    MergeFrameConsistencyErrors(protocols, out.result);
    out.result.merge(inst.validateConfigRoot(out.root, protocols));
    return Core::Expected<ConfigRootValidationOutcome>(out);
}

}} // namespace MyProt::Service
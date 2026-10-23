// src/Service/src/ConfigDeepValidator.cpp
// 配置深度校验编排层 — P2-1 拆分后的唯一入口：
//   ProtocolJsonParser.cpp  JSON → POCO
//   SchemaChecker.cpp       字段/结构规则 (§7 规则 2-11, §11, §12)
//   SemanticChecker.cpp     模板/引用链规则 (§7 规则 1/6-8/12-15)
//   TrialRenderer.cpp       帧一致性试算 (ADR-0012 §2)
// 本文件只负责：版本门禁后的规则串联、跨文件装配、JSON 文本入口。

#include "MyProt/Service/ConfigValidator.hpp"
#include "ConfigInternals.hpp"

#include <cctype>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace MyProt { namespace Service {

using detail::ParseProtocolDocImpl;
using detail::ParseRootDocImpl;

ConfigValidator::ConfigValidator(int supportedSchemaVersion)
    : _supportedVersion(supportedSchemaVersion) {}

// ── 协议层编排 (规则 1-8) ──

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

    // 协议级 simulation（可选）: 端口/寄存器数/初值/操作语义与 server.json 同一套规则。
    // responseTemplate 的行文法已在结构解析阶段验证。
    const std::vector<std::string> simErrs =
        detail::ValidateSimulationConfig(proto.simulation, proto.protocolName);
    for (std::size_t i = 0; i < simErrs.size(); ++i) r.addError(simErrs[i]);
    return r;
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

// ── JSON → POCO 解析 (文本入口, 委托 ProtocolJsonParser) ──

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

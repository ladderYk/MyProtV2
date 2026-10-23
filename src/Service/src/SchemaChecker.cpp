// src/Service/src/SchemaChecker.cpp
// 配置模式/结构校验（§7 规则 2-11 + §11 韧性 + §12 管理面）。
// 拆分自 ConfigDeepValidator.cpp（P2-1）：只回答“字段本身是否合法”，不处理跨文件引用。

#include "MyProt/Service/ConfigValidator.hpp"
#include "ConfigInternals.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "MyProt/Core/Platform.hpp"     // 跨平台文件存在性
#include "MyProt/Core/TemplateGrammar.hpp"

namespace MyProt { namespace Service {

using detail::IsHexChar;

namespace {

bool FileExists(const std::string& path) {
    return Core::Platform::FileExists(path);
}

} // namespace

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
                        // frameGapUs 必须 >= 1 — 引擎不内置 Modbus RTU 的 charTimeUs 折算默认值,
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

// ── simulation 段（协议级多实例 + server.json 旧单实例共用） ──

namespace detail {

std::vector<std::string> ValidateSimulationConfig(
        const Core::SimulationConfig& sim, const std::string& ctx) {
    std::vector<std::string> errors;
    const std::string p = ctx.empty() ? "simulation" : ctx + ".simulation";

    if (sim.registerCount < 1 || sim.registerCount > 65536) {
        errors.push_back(p + ".registerCount 须在 [1,65536], 实际 "
                          + std::to_string(sim.registerCount));
    }

    for (std::map<std::string, std::uint32_t>::const_iterator it =
             sim.initialValues.begin(); it != sim.initialValues.end(); ++it) {
        if (it->second > 65535) {
            errors.push_back(p + ".initialValues[" + it->first
                              + "] 须在 [0,65535]");
        }
        errno = 0;
        char* end = 0;
        const long addr = std::strtol(it->first.c_str(), &end, 10);
        if (it->first.empty() || end == it->first.c_str() || *end != '\0'
                || errno == ERANGE || addr < 0) {
            errors.push_back(p + ".initialValues 地址键须为非负十进制整数: \""
                              + it->first + "\"");
        } else if (addr >= sim.registerCount) {
            errors.push_back(p + ".initialValues[" + it->first
                              + "] 超出数据区 (registerCount="
                              + std::to_string(sim.registerCount) + ")");
        }
    }

    for (std::map<std::string, Core::SimOperationConfig>::const_iterator it =
             sim.operations.begin(); it != sim.operations.end(); ++it) {
        const std::string op = p + ".operations." + it->first;
        if (!it->second.kind.empty()
                && it->second.kind != "read" && it->second.kind != "write") {
            errors.push_back(op + ".kind 须为 read/write, 实际 \""
                              + it->second.kind + "\"");
        }
        if (it->second.kind == "read" && it->second.countVar.empty()) {
            errors.push_back(op + ".countVar 在读操作中必填");
        }
        if (it->second.dataOffset < -1) {
            errors.push_back(op + ".dataOffset 须 ≥ -1");
        }
    }
    return errors;
}

} // namespace detail

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
    // {Name:Xn} | {Name:raw} (P1 A 变长字节注入); 其余段数一律非法
        // (三段校验和文法不支持, 定向报错见 validateTemplate)
    return Core::TemplateGrammar::IsValidPlaceholder(s);
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

}} // namespace MyProt::Service

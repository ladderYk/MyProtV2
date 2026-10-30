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
                         "(当前版本运行期仅支持 Tcp/Udp；Serial 配置合法但通道尚未接入工厂)");
            break;
        case Core::TransportType::Serial:
            if (t.serial.baudRate == 0) r.addError("Serial baudRate 必须 > 0 (规则2)");
            break;
        case Core::TransportType::Udp:
            // UDP 无连接, 协议级无额外约束; 端口有效性由设备层 (规则10) 校验
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
            if (lf.lengthEncoding == Core::LengthFieldEncoding::Varint) {
                if (lf.lengthFieldLength != 0) {
                    r.addError("LengthField lengthFieldEncoding=Varint 时 lengthFieldLength 必须为 0 "
                               "(宽度由 varint 编码决定), 实际 "
                               + std::to_string(lf.lengthFieldLength) + " (规则3)");
                }
                if (lf.varintMaxBytes < 1 || lf.varintMaxBytes > 4) {
                    r.addError("LengthField varintMaxBytes 取值须为 1~4, 实际 "
                               + std::to_string(lf.varintMaxBytes) + " (规则3)");
                }
            } else if (lf.lengthFieldLength != 1 && lf.lengthFieldLength != 2
                    && lf.lengthFieldLength != 4) {
                r.addError("LengthField lengthFieldLength 取值须为 1/2/4, 实际 "
                           + std::to_string(lf.lengthFieldLength) + " (规则3)");
            }
            if (lf.lengthFieldOffset < 0) {
                r.addError("LengthField lengthFieldOffset 必须 ≥ 0 (规则3)");
            }
            // Varint 宽度不定, 静态只能约束最少 1 字节
            const int lengthWidth = lf.lengthEncoding == Core::LengthFieldEncoding::Varint
                ? 1 : lf.lengthFieldLength;
            if (lf.headerLength != 0
                    && lf.headerLength < lf.lengthFieldOffset + lengthWidth) {
                r.addError("headerLength 必须 ≥ lengthFieldOffset + 长度域宽度, 或为 0 (规则3)");
            }
            const int effHeader = lf.headerLength > 0
                ? lf.headerLength
                : lf.lengthFieldOffset + lengthWidth;
            if (lf.maxFrameSize <= effHeader) {
                r.addError("maxFrameSize (" + std::to_string(lf.maxFrameSize)
                           + ") 必须 > 有效帧头长度 (" + std::to_string(effHeader) + ") (规则3)");
            }

            // ADR-0021 §2.2: 位域字段
            const int64_t frameBits = static_cast<int64_t>(lf.maxFrameSize) * 8;
            for (size_t i = 0; i < lf.headerFields.size(); ++i) {
                const Core::HeaderFieldConfig& hf = lf.headerFields[i];
                const std::string ctx = "headerFields[" + std::to_string(i) + "]";
                if (hf.name.empty()) {
                    r.addError(ctx + " name 不能为空 (规则3)");
                } else {
                    for (size_t k = 0; k < i; ++k) {
                        if (lf.headerFields[k].name == hf.name) {
                            r.addError(ctx + " name 重复: \"" + hf.name + "\" (规则3)");
                            break;
                        }
                    }
                }
                if (hf.bitOffset < 0) {
                    r.addError(ctx + " bitOffset 必须 ≥ 0 (规则3)");
                }
                if (hf.bitWidth < 1 || hf.bitWidth > 32) {
                    r.addError(ctx + " bitWidth 取值须为 1~32, 实际 "
                               + std::to_string(hf.bitWidth) + " (规则3)");
                } else if (hf.bitOffset >= 0
                        && static_cast<int64_t>(hf.bitOffset) + hf.bitWidth > frameBits) {
                    r.addError(ctx + " 位域越界: bitOffset+" "bitWidth 超出 maxFrameSize (规则3)");
                }
            }

            // ADR-0021 §2.3: 分派表
            if (!lf.dispatch.field.empty()) {
                bool found = false;
                for (size_t i = 0; i < lf.headerFields.size(); ++i) {
                    if (lf.headerFields[i].name == lf.dispatch.field) { found = true; break; }
                }
                if (!found) {
                    r.addError("dispatch.field \"" + lf.dispatch.field
                               + "\" 未在 headerFields 中声明 (规则3)");
                }
                for (size_t i = 0; i < lf.dispatch.cases.size(); ++i) {
                    for (size_t k = 0; k < i; ++k) {
                        if (lf.dispatch.cases[k].value == lf.dispatch.cases[i].value) {
                            r.addError("dispatch.cases value 重复: "
                                       + std::to_string(lf.dispatch.cases[i].value) + " (规则3)");
                            break;
                        }
                    }
                }
            } else if (!lf.dispatch.cases.empty()) {
                r.addError("dispatch.cases 已声明但 dispatch.field 为空 (规则3)");
            }

            // ADR-0021 §2.4: 重复记录
            if (!lf.repeat.countField.empty()) {
                bool found = false;
                for (size_t i = 0; i < lf.headerFields.size(); ++i) {
                    if (lf.headerFields[i].name == lf.repeat.countField) { found = true; break; }
                }
                if (!found) {
                    r.addError("repeat.countField \"" + lf.repeat.countField
                               + "\" 未在 headerFields 中声明 (规则3)");
                }
                if (lf.repeat.stride <= 0) {
                    r.addError("repeat.stride 必须 > 0 (规则3)");
                }
                if (lf.repeat.offset < 0) {
                    r.addError("repeat.offset 必须 ≥ 0 (规则3)");
                }
                if (lf.repeat.maxCount <= 0) {
                    r.addError("repeat.maxCount 必须 > 0 (规则3)");
                }
                if (lf.repeat.name.empty()) {
                    r.addError("repeat.name 不能为空 (规则3)");
                }
            } else if (lf.repeat.stride != 0) {
                r.addError("repeat.stride 已声明但 repeat.countField 为空 (规则3)");
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
        case Core::TransportType::Tls:
        case Core::TransportType::Udp: {
            // 规则 10
            if (dev.connection.host.empty()) {
                r.addError("Tcp/Tls/Udp 设备 " + ctx + " connection.host 不能为空 (规则10)");
            }
            if (dev.connection.port == 0) {
                uint16_t def = 0;
                if (proto->transport.type == Core::TransportType::Tcp) {
                    def = proto->transport.tcp.defaultPort;
                } else if (proto->transport.type == Core::TransportType::Udp) {
                    def = proto->transport.udp.defaultPort;
                } else {
                    def = proto->transport.tls.defaultPort;
                }
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
    if (dev.ingest.has_value()) {
        validateIngest(dev.ingest.value(), ctx, r);
    }
}

void ConfigValidator::validateIngest(const Core::IngestConfig& ing,
                                     const std::string& ctx,
                                     ValidationResult& r) const {
    const std::string p = "设备 " + ctx + " ingest";
    if (ing.type == "mqtt") {
        const Core::MqttIngestConfig& m = ing.mqtt;
        if (m.topic.empty()) {
            r.addError(p + ".mqtt.topic 不能为空 (订阅过滤器)");
        }
        if (m.qos != 0 && m.qos != 1) {
            r.addError(p + ".mqtt.qos 仅支持 0/1 (QoS2 未实现), 实际 "
                       + std::to_string(m.qos));
        }
        if (m.keepAliveSec < 0) {
            r.addError(p + ".mqtt.keepAliveSec 必须 ≥ 0 (0 = 默认 60)");
        }
        if (m.reconnectMinMs <= 0) {
            r.addError(p + ".mqtt.reconnectMinMs 必须 > 0");
        }
        if (m.reconnectMaxMs < m.reconnectMinMs) {
            r.addError(p + ".mqtt.reconnectMaxMs 必须 ≥ reconnectMinMs");
        }
        if (m.maxSilenceMs < 0) {
            r.addError(p + ".mqtt.maxSilenceMs 必须 ≥ 0 (0 = 关闭静默看门狗)");
        }
        if (m.tagFrom != "payload" && m.tagFrom != "topic") {
            r.addError(p + ".mqtt.tagFrom 取值非法: \"" + m.tagFrom
                       + "\" (仅 payload/topic)");
        }
        if (m.tagFrom == "topic" && m.tagTopicIndex <= 0) {
            r.addError(p + ".mqtt.tagTopicIndex 在 tagFrom=topic 时必须 ≥ 1 (1-based)");
        }
        return;
    }
    if (ing.type == "tcp") {
        const Core::TcpIngestConfig& t = ing.tcp;
        if (t.port == 0) {
            r.addError(p + ".tcp.port 必须 > 0 (网关监听端口)");
        }
        if (t.maxSilenceMs < 0) {
            r.addError(p + ".tcp.maxSilenceMs 必须 ≥ 0");
        }
        Core::TransportConfig tcpTransport;
        tcpTransport.type = Core::TransportType::Tcp;
        validateFraming(t.framing, tcpTransport, r);
        return;
    }
    r.addError(p + ".type 取值非法: \"" + ing.type + "\" (仅 mqtt/tcp)");
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
    if (res.disableAfterDegraded < 0) {
        r.addError(p + "disableAfterDegraded 必须 ≥ 0 (0 = 不自动禁用)");
    }
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

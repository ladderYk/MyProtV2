// src/Engine/src/RequestBuilder.cpp
// 请求构建器实现 — 模板展开为字节流 (C++11, ADR-0010 §5)

#include "MyProt/Engine/RequestBuilder.hpp"
#include <string>
#include <vector>
#include <cstdlib>
#include <cctype>

namespace MyProt { namespace Engine {

namespace {

/// 按 ':' 分割模板段内部文本; npos 时取到结尾
std::vector<std::string> SplitSegments(const std::string& inner) {
    std::vector<std::string> segs;
    size_t pos = 0;
    while (true) {
        size_t colon = inner.find(':', pos);
        segs.push_back(inner.substr(pos, colon - pos));
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return segs;
}

/// 解析 hex 字符串 ("01 0A 0b" / "010A0B" 混合格式) → 字节流
/// 容忍空格/制表符分隔; 失败返回 false
bool ParseHexStringToBytes(const std::string& hex, Core::Bytes& out) {
    std::string compact;
    compact.reserve(hex.size());
    for (size_t i = 0; i < hex.size(); ++i) {
        if (hex[i] != ' ' && hex[i] != '\t') compact += hex[i];
    }
    if (compact.empty()) return true;                          // 空串 = 0 字节 (合法)
    if (compact.size() % 2 != 0) return false;
    for (size_t i = 0; i < compact.size(); i += 2) {
        char buf[3] = { compact[i], compact[i + 1], 0 };
        char* endp = 0;
        unsigned long v = std::strtoul(buf, &endp, 16);
        if (endp != buf + 2) return false;
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

/// 公共模板展开 (P1 A 重构): 抽离出 Build/BuildBytes 共用逻辑
/// @param variableBytesHex nullptr = 纯标量模式 (Build); 非空 = 启用 {Name:raw} (BuildBytes)
Core::Expected<Core::Bytes> RenderTemplate(
    const Core::OperationConfig& op,
    const std::unordered_map<std::string, uint32_t>& variables,
    const std::unordered_map<std::string, std::string>* variableBytesHex,
    const std::unordered_map<std::string, std::string>& varAliasMap,
    AutoComputeProvider& autoProvider) {

    Core::Bytes out;

    for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
        const std::string& part = op.requestTemplate[i];
        if (part.empty()) continue;

        if (part.size() >= 2 && part[0] == '{' && part[part.size() - 1] == '}') {
            // {Name:Xn} 或 {Name:raw}; auto 行为由 variables.source=auto 声明驱动 (v1.16 去 :auto: 令牌)
            std::string inner = part.substr(1, part.size() - 2);
            std::vector<std::string> segs = SplitSegments(inner);

            std::string name = segs[0];
            // v1.27: 变量别名解析 — 用户自定义名 → 引擎内部名
            auto ait = varAliasMap.find(name);
            if (ait != varAliasMap.end()) name = ait->second;

            // P1 A: {Name:raw} 变长字节注入
            if (segs.size() == 2 && segs[1] == "raw") {
                if (!variableBytesHex) {
                    return Core::Unexpected(Core::Error::Code::BuildError,
                                            "模板段 {Name:raw} 仅 BuildBytes 入口支持", part);
                }
                std::unordered_map<std::string, std::string>::const_iterator it =
                    variableBytesHex->find(name);
                if (it == variableBytesHex->end()) {
                    return Core::Unexpected(Core::Error::Code::BuildError,
                                            "变长模板变量未提供", name);
                }
                if (!ParseHexStringToBytes(it->second, out)) {
                    return Core::Unexpected(Core::Error::Code::BuildError,
                                            "variableBytesHex 值非法 (须为偶数位十六进制)", part);
                }
                continue;
            }

            // v1.16: 模板函数令牌收敛为 CRC/LRC/XOR; :auto: 已移除, auto 行为由声明驱动
            std::string widthSpec;
            if (segs.size() == 2) {
                widthSpec = segs[1];
            } else {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "模板段格式无效 (仅 {Name:Xn} / {Name:raw})", part);
            }

            if (widthSpec.size() < 2 || widthSpec[0] != 'X') {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "宽度说明须为 Xn", part);
            }
            int hexWidth = std::atoi(widthSpec.c_str() + 1);
            if (hexWidth <= 0 || hexWidth % 2 != 0 || hexWidth > 16) {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "宽度 n 须为 1..8 字节对应的偶数 hex 位数", part);
            }
            int byteWidth = hexWidth / 2;

            uint64_t value;
            std::unordered_map<std::string, uint32_t>::const_iterator it = variables.find(name);
            if (it != variables.end()) {
                value = it->second;                                  // 静态值 / 标签值优先
            } else if (autoProvider.IsDeclared(name)) {              // v1.16: 按声明路由到 auto 策略
                BuildContext ctx;
                ctx.variables = &variables;
                ctx.frameSoFar = &out;
                value = autoProvider.Resolve(name, byteWidth, ctx);
            } else {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "模板变量未提供 (须在 variables 声明 static/auto)", name);
            }

            // v1.25 (ADR-0012 附录A 追加): 溢出拦截 — 派生值超出格式宽度时高位原被
            // 静默截断 (如 {PDULength:X4} 上限 65535, 大载荷下帧长溢出), 现显式失败
            const uint64_t maxVal = (byteWidth >= 8)
                ? ~0ULL
                : ((1ULL << (8 * byteWidth)) - 1ULL);
            if (value > maxVal) {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "变量值 " + std::to_string(value)
                                            + " 超出格式宽度 X" + std::to_string(hexWidth)
                                            + " (上限 " + std::to_string(maxVal) + ")",
                                        name);
            }

            for (int b = byteWidth - 1; b >= 0; --b) {
                out.push_back(static_cast<uint8_t>((value >> (b * 8)) & 0xFF));
            }
        } else {
            // 十六进制字面量: 每 2 字符 1 字节; 空格/制表符为字节分隔符
            // (与 ConfigDeepValidator §3.1 / TemplateMatcher 约定一致)
            std::string hex;
            hex.reserve(part.size());
            for (size_t c = 0; c < part.size(); ++c) {
                if (part[c] != ' ' && part[c] != '\t') {
                    hex += part[c];
                }
            }
            if (hex.empty()) continue;
            if (hex.size() % 2 != 0) {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "十六进制字面量长度须为偶数", part);
            }
            for (size_t c = 0; c < hex.size(); c += 2) {
                char buf[3] = { hex[c], hex[c + 1], 0 };
                char* end = 0;
                unsigned long v = std::strtoul(buf, &end, 16);
                if (end != buf + 2) {
                    return Core::Unexpected(Core::Error::Code::BuildError,
                                            "非法十六进制字符", part);
                }
                out.push_back(static_cast<uint8_t>(v));
            }
        }
    }

    return out;
}

} // namespace

Core::Expected<Core::Bytes> RequestBuilder::Build(
    const Core::OperationConfig& op,
    const std::unordered_map<std::string, uint32_t>& variables,
    const std::unordered_map<std::string, std::string>& varAliasMap,
    AutoComputeProvider& autoProvider) {
    return RenderTemplate(op, variables, nullptr, varAliasMap, autoProvider);
}

Core::Expected<Core::Bytes> RequestBuilder::BuildBytes(
    const Core::OperationConfig& op,
    const std::unordered_map<std::string, uint32_t>& variables,
    const std::unordered_map<std::string, std::string>& variableBytesHex,
    const std::unordered_map<std::string, std::string>& varAliasMap,
    AutoComputeProvider& autoProvider) {
    return RenderTemplate(op, variables, &variableBytesHex, varAliasMap, autoProvider);
}

// v1.1 增: 合并协议级 defaultVariables + 标签级 variables
// 协议 default 为基, 标签同名键覆盖, 未覆盖键继承协议默认
std::unordered_map<std::string, uint32_t> RequestBuilder::MergeVariables(
    const std::unordered_map<std::string, uint32_t>& protocolDefaults,
    const std::unordered_map<std::string, uint32_t>& tagVariables) {
    std::unordered_map<std::string, uint32_t> merged = protocolDefaults;
    for (auto it = tagVariables.begin(); it != tagVariables.end(); ++it) {
        merged[it->first] = it->second;   // 标签同名键覆盖
    }
    return merged;
}

// v1.10 增: 协议级 + op 级 + 标签级 三段合并. 优先级 标签 > op > 协议.
std::unordered_map<std::string, uint32_t> RequestBuilder::MergeVariables(
    const std::unordered_map<std::string, uint32_t>& protocolDefaults,
    const std::unordered_map<std::string, uint32_t>& opStaticVariables,
    const std::unordered_map<std::string, uint32_t>& tagVariables) {
    std::unordered_map<std::string, uint32_t> merged = protocolDefaults;
    for (auto it = opStaticVariables.begin(); it != opStaticVariables.end(); ++it) {
        merged[it->first] = it->second;   // op 同名键覆盖协议默认
    }
    for (auto it = tagVariables.begin(); it != tagVariables.end(); ++it) {
        merged[it->first] = it->second;   // 标签同名键覆盖
    }
    return merged;
}

// v1.11: 从协议 inputs 段提取 source=static 条目 (旧 protocol.defaultVariables / v1.16 protocol.variables 语义)
std::unordered_map<std::string, uint32_t> RequestBuilder::CollectStaticVariables(
    const Core::ProtocolConfig& protocol) {
    std::unordered_map<std::string, uint32_t> out;
    for (auto it = protocol.inputs.begin(); it != protocol.inputs.end(); ++it) {
        if (it->second.isStatic() && it->second.value.has_value()) {
            out[it->first] = it->second.value.value();
        }
    }
    return out;
}

// v1.11: 从协议 inputs 段重建 autoComputeJson (旧 protocol.autoComputeJson 语义);
//   outputs(derivedLength) 由参数层预解析特判注入, 不入此段 (求值时机依赖载荷字节数).
std::string RequestBuilder::CollectAutoComputeJson(const Core::ProtocolConfig& protocol) {
    std::string out;
    for (auto it = protocol.inputs.begin(); it != protocol.inputs.end(); ++it) {
        const Core::VariableConfig& v = it->second;
        if (!v.isAuto() || v.isDerivedLength()) continue;
        if (!out.empty()) out += ",";
        out += "\"" + it->first + "\":{\"strategy\":\"" + v.strategy + "\"";
        if (!v.paramsJson.empty()) {
            out += ",\"params\":" + v.paramsJson;
        }
        out += "}";
    }
    if (out.empty()) return std::string();
    return "{" + out + "}";
}

// op 级 auto (非 derivedLength) 输入拼接进协议级 autoComputeJson。
// 单一实现 — Gateway(TagReader) 与 Service(FrameConsistencyCheck) 共用,
// 消除原双份实现 (Gateway 未跳过 derivedLength, 与 Service 语义漂移)。
std::string RequestBuilder::MergeOpAutoComputeJson(
        const Core::ProtocolConfig& protocol, const Core::OperationConfig& op) {
    std::string base = CollectAutoComputeJson(protocol);
    if (op.inputs.empty()) return base;
    std::string opPart;
    for (auto it = op.inputs.begin(); it != op.inputs.end(); ++it) {
        const Core::VariableConfig& v = it->second;
        if (!v.isAuto() || v.isDerivedLength()) continue;
        if (!opPart.empty()) opPart += ",";
        opPart += "\"" + it->first + "\":{";
        opPart += "\"strategy\":\"" + v.strategy + "\"";
        if (!v.paramsJson.empty()) {
            opPart += ",\"params\":" + v.paramsJson;
        }
        opPart += "}";
    }
    if (opPart.empty()) return base;
    if (base.empty()) return "{" + opPart + "}";
    if (base.back() == '}') {
        base.pop_back();
        return base + "," + opPart + "}";
    }
    return "{" + opPart + "}";
}

// ── v1.25 (ADR-0012): 模板布局与派生长度注入 ──────────────────────────────

// 扫描 requestTemplate 产出布局表. 元素二分法与 RenderTemplate 一致:
// 整元素占位符 ({...}) 或 hex 字面量; 未知格式记 hasUnknown (宽度按 0).
TemplateLayout RequestBuilder::BuildTemplateLayout(
    const std::vector<std::string>& requestTemplate) {
    TemplateLayout out;
    uint32_t cursor = 0;
    for (size_t i = 0; i < requestTemplate.size(); ++i) {
        const std::string& el = requestTemplate[i];
        if (el.empty()) continue;
        uint32_t w = 0;
        const bool isPlaceholder =
            (el.size() >= 2 && el[0] == '{' && el[el.size() - 1] == '}');
        if (isPlaceholder) {
            const std::string inner = el.substr(1, el.size() - 2);
            const size_t colon = inner.find(':');
            if (colon == std::string::npos) {
                out.hasUnknown = true;                        // 无格式段 (如 "{Name}")
            } else {
                const std::string name = inner.substr(0, colon);
                const std::string fmt = inner.substr(colon + 1);
                if      (fmt == "X2")  w = 1;
                else if (fmt == "X4")  w = 2;
                else if (fmt == "X8")  w = 4;
                else if (fmt == "X16") w = 8;
                else if (fmt == "raw") w = TemplateLayout::kRawMarker;
                else out.hasUnknown = true;
                if (out.offsets.find(name) == out.offsets.end()) {
                    out.offsets[name] = cursor;               // 首现偏移
                }
                if (out.widths.find(name) == out.widths.end()) {
                    out.widths[name] = w;                     // 首现宽度
                }
                if (w != TemplateLayout::kRawMarker) out.fixedTotal += w;
            }
        } else {
            // hex 字面量: 非空白字符两两成字节 (与 RenderTemplate/校验器 §3.1 约定一致)
            size_t hexChars = 0;
            for (size_t c = 0; c < el.size(); ++c) {
                const char ch = el[c];
                if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r') ++hexChars;
            }
            w = static_cast<uint32_t>(hexChars / 2);
            out.fixedTotal += w;
        }
        if (w != TemplateLayout::kRawMarker) cursor += w;
    }
    return out;
}

// 派生长度变量注入 (原 Gateway::TagReader 匿名实现上移, v1.11 语义不变):
// protocolOutputs ∪ opOutputs 中 derivedLength 声明按 expr 求值注入;
// {name:len} 宽度表来自 layout (raw 标记位替换为实际载荷字节数).
void RequestBuilder::InjectDerivedLengthVariables(
        std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, Core::VariableConfig>& protocolOutputs,
        const std::unordered_map<std::string, Core::VariableConfig>& opOutputs,
        std::size_t totalBytes,
        const TemplateLayout& layout) {
    std::unordered_map<std::string, const Core::VariableConfig*> outputs;
    for (auto& kv : protocolOutputs) outputs[kv.first] = &kv.second;
    for (auto& kv : opOutputs)       outputs[kv.first] = &kv.second;

    std::unordered_map<std::string, uint32_t> varLen;
    for (auto& kv : layout.widths) {
        if (kv.second == TemplateLayout::kRawMarker) {
            varLen[kv.first] = static_cast<uint32_t>(totalBytes);
        } else {
            varLen[kv.first] = kv.second;
        }
    }

    for (auto& kv : outputs) {
        const Core::VariableConfig& v = *kv.second;
        if (!v.isDerivedLength()) continue;
        if (variables.find(kv.first) != variables.end()) continue;  // 显式优先
        uint32_t val = 0;
        std::string derr;
        if (AutoComputeProvider::ResolveDerivedLength(
                v.expr, &variables, &varLen, &layout, val, &derr)) {
            variables[kv.first] = val;
        } else {
            // v1.27: 派生值求值失败 — 模板渲染期将报 "模板变量未提供", 此处记录原因供诊断
            // (静默跳过, 不在构建期打断请求; 渲染期错误更直观)
            (void)derr; // 未用, 保留供调试
        }
    }
}

}} // namespace MyProt::Engine

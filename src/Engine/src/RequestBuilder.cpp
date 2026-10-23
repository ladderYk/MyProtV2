// src/Engine/src/RequestBuilder.cpp
// 请求构建器实现 — 模板展开为字节流 (C++11, ADR-0010 §5)

#include "MyProt/Engine/RequestBuilder.hpp"
#include "MyProt/Core/TemplateGrammar.hpp"
#include <string>
#include <vector>

namespace MyProt { namespace Engine {

namespace {

/// 公共模板展开: Build/BuildBytes 共用逻辑。
/// 文法解析统一委托 Core::TemplateGrammar (与仿真/加载期校验同一实现)。
/// @param variableBytesHex nullptr = 纯标量模式 (Build); 非空 = 启用 {Name:raw} (BuildBytes)
Core::Expected<Core::Bytes> RenderTemplate(
    const Core::OperationConfig& op,
    const std::unordered_map<std::string, uint32_t>& variables,
    const std::unordered_map<std::string, std::string>* variableBytesHex,
    const std::unordered_map<std::string, std::string>& varAliasMap,
    AutoComputeProvider& autoProvider) {

    namespace TG = Core::TemplateGrammar;
    Core::Bytes out;

    for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
        const std::string& part = op.requestTemplate[i];
        if (part.empty()) continue;

        TG::Segment seg;
        std::string parseErr;
        const TG::ParseStatus st = TG::ParseElement(part, seg, false, false, &parseErr);
        if (st != TG::ParseOk) {
            return Core::Unexpected(Core::Error::Code::BuildError,
                                    parseErr.empty() ? "模板段格式无效" : parseErr,
                                    part);
        }

        if (seg.kind == TG::kLiteral) {
            out.insert(out.end(), seg.literal.begin(), seg.literal.end());
            continue;
        }

        // 变量别名解析 — 用户自定义名 → 引擎内部名
        std::string name = seg.name;
        std::unordered_map<std::string, std::string>::const_iterator ait =
            varAliasMap.find(name);
        if (ait != varAliasMap.end()) name = ait->second;

        // {Name:raw} 变长字节注入
        if (seg.kind == TG::kRaw) {
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
            if (!TG::ParseHexLiteral(it->second, out)) {
                return Core::Unexpected(Core::Error::Code::BuildError,
                                        "variableBytesHex 值非法 (须为偶数位十六进制)", part);
            }
            continue;
        }

        // {Name:Xn} / {Name:XnLE} — 宽度与线序由文法层给出
        const int byteWidth = seg.widthBytes;
        uint64_t value;
        std::unordered_map<std::string, uint32_t>::const_iterator it = variables.find(name);
        if (it != variables.end()) {
            value = it->second;                                  // 静态值 / 标签值优先
        } else if (autoProvider.IsDeclared(name)) {              // 按声明路由到 auto 策略
            BuildContext ctx;
            ctx.variables = &variables;
            ctx.frameSoFar = &out;
            value = autoProvider.Resolve(name, byteWidth, ctx);
        } else {
            return Core::Unexpected(Core::Error::Code::BuildError,
                                    "模板变量未提供 (须在 variables 声明 static/auto)", name);
        }

        // 溢出拦截 (ADR-0012 附录A): 派生值超出格式宽度时显式失败 —
        // 不允许高位静默截断 (如 {PDULength:X4} 超 65535 会让帧长字段偏小, 设备侧解析错位)
        const uint64_t maxVal = (byteWidth >= 8)
            ? ~0ULL
            : ((1ULL << (8 * byteWidth)) - 1ULL);
        if (value > maxVal) {
            return Core::Unexpected(Core::Error::Code::BuildError,
                                    "变量值 " + std::to_string(value)
                                        + " 超出格式宽度 X" + std::to_string(byteWidth * 2)
                                        + " (上限 " + std::to_string(maxVal) + ")",
                                    name);
        }

        if (seg.littleEndian) {
            for (int b = 0; b < byteWidth; ++b) {
                out.push_back(static_cast<uint8_t>((value >> (b * 8)) & 0xFF));
            }
        } else {
            for (int b = byteWidth - 1; b >= 0; --b) {
                out.push_back(static_cast<uint8_t>((value >> (b * 8)) & 0xFF));
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

// 合并协议级 defaultVariables + 标签级 variables
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

// 协议级 + op 级 + 标签级 三段合并. 优先级 标签 > op > 协议.
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

// 从协议 inputs 段提取 source=static 条目 (协议级变量基准; 配置形态无单段 variables — 见 ADR-0005)
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

// 从协议 inputs 段重建 autoComputeJson (auto 声明的唯一来源是 inputs.source=auto);
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
// 单一实现 — Gateway(TagReader) 与 Service(TrialRenderer) 共用,
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

// ── 模板布局与派生长度注入 (ADR-0012) ──────────────────────────────

// 扫描 requestTemplate 产出布局表. 元素文法统一来自 Core::TemplateGrammar:
// 整元素占位符 ({...}) 或 hex 字面量; 未知格式记 hasUnknown (宽度按 0).
TemplateLayout RequestBuilder::BuildTemplateLayout(
    const std::vector<std::string>& requestTemplate) {
    namespace TG = Core::TemplateGrammar;
    TemplateLayout out;
    uint32_t cursor = 0;
    for (size_t i = 0; i < requestTemplate.size(); ++i) {
        const std::string& el = requestTemplate[i];
        if (el.empty()) continue;

        TG::Segment seg;
        const TG::ParseStatus st = TG::ParseElement(el, seg, false, false, 0);
        const bool isPlaceholder =
            (el.size() >= 2 && el[0] == '{' && el[el.size() - 1] == '}');
        uint32_t w = 0;

        if (isPlaceholder) {
            if (st == TG::ParseOk && seg.kind == TG::kRaw) {
                w = TemplateLayout::kRawMarker;
            } else if (st == TG::ParseOk) {
                w = static_cast<uint32_t>(seg.widthBytes);
            } else {
                out.hasUnknown = true;                        // 无格式段 (如 "{Name}") / 未知格式
            }
            // 解析失败时 seg.name 为空, 从文法层重新取首段名 (未知格式仍记录首现位置)
            const std::vector<std::string> parts =
                TG::SplitSegments(el.substr(1, el.size() - 2));
            const std::string name = parts.empty() ? std::string() : parts[0];
            if (!name.empty()) {
                if (out.offsets.find(name) == out.offsets.end()) {
                    out.offsets[name] = cursor;               // 首现偏移
                }
                if (out.widths.find(name) == out.widths.end()) {
                    out.widths[name] = w;                     // 首现宽度
                }
            }
            if (w != TemplateLayout::kRawMarker) out.fixedTotal += w;
        } else {
            // hex 字面量: 非空白字符两两成字节
            if (st == TG::ParseOk) {
                w = static_cast<uint32_t>(seg.literal.size());
            } else {
                out.hasUnknown = true;                        // 非法 hex 字面量
            }
            out.fixedTotal += w;
        }
        if (w != TemplateLayout::kRawMarker) cursor += w;
    }
    return out;
}

// 派生长度变量注入 (唯一实现在 Engine, Gateway/Service 共用同一份语义):
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
            // 派生值求值失败 — 模板渲染期将报 "模板变量未提供", 此处记录原因供诊断
            // (不在构建期打断请求; 渲染期错误更直观)
            (void)derr; // 未用, 保留供调试
        }
    }
}

}} // namespace MyProt::Engine

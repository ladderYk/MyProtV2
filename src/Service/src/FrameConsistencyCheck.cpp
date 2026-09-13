// src/Service/src/FrameConsistencyCheck.cpp
// 保存期试算校验 (ADR-0012 §2) — 对含 {Name:raw} 载荷的写操作以真实管线试渲染一帧:
//   1) 派生长度 expr 可解性: 求值失败/引用未知变量 → 错误 (封堵运行时静默 0 值路径,
//      即 v1.13 自检 known=false → continue 的缺口 A);
//   2) 帧长一致性: 渲染帧按 framing 语义反推长度槽位期望值, 与槽位实际字节比对 —
//      同时捕获 "模板改了固定段、outputs 魔数没跟" 与 "framing 误配" 两类错误;
//   3) {Frame:fixed} 保留名校验: 模板占位符不得命名 Frame.
// 装配管线与 Gateway::TagReader::WriteBytes 完全一致 (InjectDerivedLengthVariables
// 已上移 Engine, 两处共用同一实现, 无双实现漂移)。
// 挂接点: 保存期 ConfigStore::Validate + 加载期 validateConfigRootJson /
//   validateAndParseConfigRootJson (两条入口同一实现, 手工编辑或外部生成的配置
//   不再绕过门禁)。

#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Config.hpp"   // kFramePrimitiveName

#include <set>
#include <sstream>

namespace MyProt { namespace Service {

namespace {

/// 模板是否包含 {Name:raw} 载荷占位符
bool HasRawPlaceholder(const std::vector<std::string>& tpl) {
    for (size_t i = 0; i < tpl.size(); ++i) {
        if (tpl[i].find(":raw}") != std::string::npos) return true;
    }
    return false;
}

/// 提取首个 {Name:raw} 的载荷变量名 (空 = 无整元素 raw; 混合行不支持)
std::string FirstRawName(const std::vector<std::string>& tpl) {
    for (size_t i = 0; i < tpl.size(); ++i) {
        const std::string& el = tpl[i];
        if (el.size() < 6 || el[0] != '{' || el[el.size() - 1] != '}') continue;
        if (el.compare(el.size() - 5, 5, ":raw}") != 0) continue;
        return el.substr(1, el.size() - 6);
    }
    return std::string();
}

} // namespace

std::vector<std::string> ConfigValidator::CheckFrameConsistency(
    const Core::ProtocolConfig& proto) {

    std::vector<std::string> errs;
    // 仅 LengthField 成帧 + 含 {Name:raw} 的写操作参与检查 (§2.1);
    // 读操作/定长写不涉及派生帧长, 不检查 (避免动态长度字段误报)。
    if (proto.framing.type != Core::FramingType::LengthField) return errs;
    if (proto.framing.lengthField.lengthFieldLength <= 0) return errs;

    for (auto it = proto.operations.begin(); it != proto.operations.end(); ++it) {
        const Core::OperationConfig& op = it->second;
        if (!HasRawPlaceholder(op.requestTemplate)) continue;
        const std::string opName = op.name.empty() ? it->first : op.name;
        const std::string tag = " (ADR-0012 §2)";

        // 0. {Frame:fixed} 保留名 — 模板占位符不得命名 Frame
        const Engine::TemplateLayout layout =
            Engine::RequestBuilder::BuildTemplateLayout(op.requestTemplate);
        if (layout.widths.find(Core::kFramePrimitiveName) != layout.widths.end()) {
            errs.push_back("操作 " + opName
                + " 模板占位符不得命名 \"Frame\" (表达式保留名)" + tag);
        }
        if (layout.hasUnknown) {
            errs.push_back("操作 " + opName
                + " 模板含未知格式占位符, 布局不可核算" + tag);
            continue;
        }

        // 1. 变量表 = 协议 static ∪ op static(有值) ∪ StartByteAddress=0
        //    (与 TagReader::WriteBytes 装配链一致, 仅无标签级覆盖 — 试算无标签上下文;
        //     v1.25: 注入跨协议字节单位, 协议族地址由 outputs 派生)
        std::unordered_map<std::string, uint32_t> variables =
            Engine::RequestBuilder::CollectStaticVariables(proto);
        for (auto vit = op.inputs.begin(); vit != op.inputs.end(); ++vit) {
            if (vit->second.isStatic() && vit->second.value.has_value()) {
                variables[vit->first] = vit->second.value.value();
            }
        }
        variables[Core::StartByteAddressVariableName()] = 0;
        variables[Core::ByteCountVariableName()] = 2;

        // 2. 派生长度注入 (dummy 载荷 3 字节) + expr 可解性预检 —
        //    注入后任一 derivedLength 声明未进入变量表 = expr 求值失败
        //    (语法错 / 引用未知变量 / 引用未知原语), 显式报错而非渲染期泛化 BuildError
        static const std::size_t kDummyPayloadBytes = 3;
        Engine::RequestBuilder::InjectDerivedLengthVariables(
            variables, proto.outputs, op.outputs, kDummyPayloadBytes, layout);
        {
            std::set<std::string> declNames;
            for (auto oit = proto.outputs.begin(); oit != proto.outputs.end(); ++oit)
                if (oit->second.isDerivedLength()) declNames.insert(oit->first);
            for (auto oit = op.outputs.begin(); oit != op.outputs.end(); ++oit)
                if (oit->second.isDerivedLength()) declNames.insert(oit->first);
            for (auto nit = declNames.begin(); nit != declNames.end(); ++nit) {
                if (variables.find(*nit) == variables.end()) {
                    errs.push_back("操作 " + opName + " 派生长度 \"" + *nit
                        + "\" expr 求值失败 (语法错误或引用了未知变量/原语), "
                          "渲染时该字段将缺失" + tag);
                }
            }
        }

        // 3. 试渲染一帧 (autoIncrement 变量经 DeclareJson 走真实策略路径)
        Engine::AutoComputeProvider autoProv;
        const std::string acJson =
            Engine::RequestBuilder::MergeOpAutoComputeJson(proto, op);
        if (!acJson.empty()) autoProv.DeclareJson(acJson);

        std::string rawName = FirstRawName(op.requestTemplate);
        if (rawName.empty()) {
            errs.push_back("操作 " + opName
                + " 变长载荷 {Name:raw} 占位符须独占模板元素" + tag);
            continue;
        }
        std::unordered_map<std::string, std::string> rawVars;
        rawVars[rawName] = "01 02 03";

        Engine::RequestBuilder builder;
        auto built = builder.BuildBytes(op, variables, rawVars, {}, autoProv);
        if (!built.has_value()) {
            errs.push_back("操作 " + opName + " 试渲染失败: "
                + built.error().message + tag);
            continue;
        }
        const Core::Bytes& frame = built.value();

        // 4. 长度槽位一致性: expected = includesHeader ? total-adj
        //                                            : total-headerLength-adj
        const Core::LengthFieldConfig& lf = proto.framing.lengthField;
        const size_t need = static_cast<size_t>(lf.lengthFieldOffset)
                          + static_cast<size_t>(lf.lengthFieldLength);
        if (frame.size() < need) {
            errs.push_back("操作 " + opName + " 渲染帧 ("
                + std::to_string(frame.size())
                + "B) 短于长度槽位要求 (" + std::to_string(need) + "B)" + tag);
            continue;
        }
        const uint8_t* p = frame.data() + lf.lengthFieldOffset;
        uint64_t lenVal = 0;
        const bool bigEndian = (lf.byteOrder == Core::ByteOrder::BigEndian);
        switch (lf.lengthFieldLength) {
            case 1: lenVal = p[0]; break;
            case 2: lenVal = bigEndian
                ? ((static_cast<uint64_t>(p[0]) << 8) | p[1])
                : ((static_cast<uint64_t>(p[1]) << 8) | p[0]);
                break;
            case 4: lenVal = bigEndian
                ? ((static_cast<uint64_t>(p[0]) << 24) | (static_cast<uint64_t>(p[1]) << 16)
                   | (static_cast<uint64_t>(p[2]) << 8)  | p[3])
                : ((static_cast<uint64_t>(p[3]) << 24) | (static_cast<uint64_t>(p[2]) << 16)
                   | (static_cast<uint64_t>(p[1]) << 8)  | p[0]);
                break;
            default: continue;   // 校验器规则 3 已拦截非法宽度
        }
        const long long total = static_cast<long long>(frame.size());
        const long long expected = lf.lengthIncludesHeader
            ? total - lf.lengthAdjustment
            : total - lf.headerLength - lf.lengthAdjustment;
        if (static_cast<long long>(lenVal) != expected) {
            std::ostringstream oss;
            oss << "操作 " << opName << " 帧长一致性失败: 长度槽位值 " << lenVal
                << " ≠ 期望 " << expected << " (渲染总长 " << total << "B, 载荷 "
                << kDummyPayloadBytes << "B); 模板固定段与派生长度/成帧配置不一致"
                << tag;
            errs.push_back(oss.str());
        }
    }
    return errs;
}

}} // namespace MyProt::Service

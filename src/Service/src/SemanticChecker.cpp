// src/Service/src/SemanticChecker.cpp
// 配置语义/跨文件校验（§7 规则 1/6-8/12-15 + 跨文件引用）。
// 拆分自 ConfigDeepValidator.cpp（P2-1）：模板文法、派生长度自检、标签与配置根引用链。

#include "MyProt/Service/ConfigValidator.hpp"
#include "ConfigInternals.hpp"

#include <cctype>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "MyProt/Core/TemplateGrammar.hpp"

namespace MyProt { namespace Service {

using detail::CollectRawPlaceholderNames;
using detail::IsRawSpec;
using detail::ProtocolResolvesInputVariable;
using detail::TemplateElementBytes;
using detail::TemplateElementName;
using detail::TryPayloadOffset;

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

// 长度偏移自检: 见上方 namespace 辅助; 对 protocol.outputs(可被 op 覆盖) 中
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
        // 载荷名 = 模板 {Name:raw} 占位符名 (无需 inputs 特殊声明)
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
                // 槽位变量: op 级 outputs 覆盖优先, 否则协议级 outputs (派生输出在 outputs 段)
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
                if (!v->expr.empty()) {                       // 仅 expr (kind 不属于本层契约)
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

/// 三段文法 {Name:seg2:Xn} 不受支持 → 定向报错并指引迁移路径 (否则会退化成
/// 泛化的"既非十六进制字面量也非合法占位符", 用户无从知道该改什么)
static bool RejectRemovedThreeSegmentSyntax(const std::string& opName,
                                            const std::string& line,
                                            const std::string& tok,
                                            ValidationResult& r) {
    std::vector<std::string> parts;
    if (!Core::TemplateGrammar::SplitPlaceholder(tok, parts) || parts.size() != 3) return false;
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
// ── 标签层 (规则 12-15) ──

void ConfigValidator::validateTag(const Core::TagDefinition& tag,
                                  const std::unordered_map<std::string,
                                      const Core::DeviceConfig*>& deviceMap,
                                  const std::unordered_map<std::string,
                                      const Core::ProtocolConfig*>& protoMap,
                                  ValidationResult& r) const {
    const std::string ctx = tag.name.empty() ? std::string("(匿名标签)") : tag.name;

    // 规则 13
        // 写标签 (direction=write): 不参与轮询, scanRateMs 不作要求;
    //       readBackTag 仅写标签可配; writeVariable 不可为空.
    const bool isWriteTag = (tag.direction == "write");
    const bool isPushTag = (tag.source == "push");
    if (tag.direction != "read" && !isWriteTag) {
        r.addError("标签 " + ctx + " direction 取值非法: \"" + tag.direction
                   + "\" (仅 read/write) (规则13)");
    }
    if (tag.source != "poll" && !isPushTag) {
        r.addError("标签 " + ctx + " source 取值非法: \"" + tag.source
                   + "\" (仅 poll/push) (规则13)");
    }
    if (isPushTag && isWriteTag) {
        r.addError("标签 " + ctx + " source=push 与 direction=write 互斥 (规则13)");
    }
    if (isPushTag) {
        // push 标签由 ingest 适配器产出: 不经操作模板、不参与轮询
        if (!tag.operation.empty()) {
            r.addError("标签 " + ctx + " source=push 不得声明 operation (不经模板解析) (规则13)");
        }
        if (tag.scanRateMs != 0) {
            r.addWarning("标签 " + ctx + " source=push 的 scanRateMs 将被忽略 (规则13)");
        }
        std::unordered_map<std::string, const Core::DeviceConfig*>::const_iterator pdev =
            deviceMap.find(tag.deviceId);
        if (pdev == deviceMap.end()) {
            r.addError("标签 " + ctx + " 引用不存在的设备: \"" + tag.deviceId
                       + "\" (规则12)");
        } else if (!pdev->second->ingest.has_value()) {
            r.addError("标签 " + ctx + " source=push 但设备 \"" + tag.deviceId
                       + "\" 未配置 ingest (规则13)");
        }
    } else if (!isWriteTag && tag.scanRateMs <= 0) {
        r.addError("标签 " + ctx + " scanRateMs 必须 > 0 (规则13)");
    }
    if (isWriteTag && tag.writeVariable.empty()) {
        r.addError("标签 " + ctx + " 写标签 writeVariable 不能为空 (规则13)");
    }
    if (!isWriteTag && !tag.readBackTag.empty()) {
        r.addError("标签 " + ctx + " readBackTag 仅写标签 (direction=write) 可配置 (规则13)");
    }
        // 标签级写能力: writeOperation/writeBytesOperation/writeVariables
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
    if (!(isPushTag && tag.finalType == "Auto") && !isAllowedFinalType(tag.finalType)) {
        r.addError("标签 " + ctx + " finalType 取值非法: \"" + tag.finalType
                   + "\" (规则13, 见 Config_Schema §6; push 标签可用 Auto)");
    }

        // v5 (C1): converters 校验 — 仅读标签 + 仅数值型 finalType.
    //   Bool/ByteArray/String 无"乘加"语义; 写标签值由调用方给定, 不经换算.
    if (!tag.converters.empty()) {
        if (isWriteTag) {
            r.addError("标签 " + ctx + " 写标签 (direction=write) 不可声明 converters (规则13)");
        }
        const std::string& ft = tag.finalType;
        const bool numeric =
            ft == "UInt16" || ft == "Int16" || ft == "UInt32" || ft == "Int32"
            || ft == "UInt64" || ft == "Int64" || ft == "Float" || ft == "Double"
            || (isPushTag && ft == "Auto");   // Auto 按 payload 原生类型推断, 数值型时换算链生效
        if (!numeric) {
            r.addError("标签 " + ctx + " converters 仅支持数值型 finalType, 当前: \""
                       + ft + "\" (规则13)");
        }
    }

        // 位粒度: BitOffset 约定键校验 (语义 = 字节内位偏移 0-7).
    //   Error: 超范围 (运行时 ResponseParser 亦有防御);
    //   Warning: 声明 BitOffset 但 finalType 非 Bool (当前无消费方);
    //            Bool 未声明 BitOffset (位寻址 op 需显式声明, 否则解析走
    //            raw[0]!=0 整寄存器真假, 线圈场景会误报同字节其他位);
    //            Bool + BitOffset + ByteCount > 1 (合法但疑似笔误, 位标签跨度应为 1).
    {
                // 位偏移读标签一等字段 tag.bitOffset (-1 = 未声明); 不查 variables 魔法键
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

        // 规则 15 Warning — 跨协议字节单位 "ByteCount" (约定键).
    //   与 tags.json 中读取标签声明的 {ByteCount:N} 与协议 JSON 协议族单位 {RegisterCount:M} (Modbus)
//     一致性由协议 JSON derivedLength 自身保证, 此处不再做跨字段核对 (避免把协议族知识渗入校验).
    //   仍保留 {StartByteAddress} > 0 的合理性警告 (按字节地址线性推断).
    if (isPushTag) {
        // push 标签不经 operation/模板/占位符链路: 来源绑定与类型已在上面校验完毕
        return;
    }

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
        // 操作 kind 语义一致性 (kind 未标注 = 跳过检查)
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
        // 只发操作 (expectResponse=false): 发送后不读取应答.
    //   读标签 operation 引用只发操作 = 永远拿不到数据 (Error);
    //   只发操作只能用于写类语义.
    if (!oit->second.expectResponse) {
        if (!isWriteTag) {
            r.addError("标签 " + ctx + " operation 引用只发操作 (expectResponse=false): \""
                       + tag.operation + "\"; 读标签必须使用请求-应答操作 (规则12)");
        } else if (!oit->second.kind.empty() && oit->second.kind != "write") {
            r.addError("标签 " + ctx + " operation 引用只发操作 (expectResponse=false) 但 kind="
                       + oit->second.kind + "; 只发操作须为 write 类 (规则12)");
        }
    }
        // writeOperation 引用存在 + kind 一致性 (规则12)
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
        // writeBytesOperation 引用存在 + kind 一致性 (规则12)
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
        // 运行时注入的变量加入解析域白名单 —
        //   writeVariable (数值/字节注入) + 起始地址 (固定 "StartAddress"; addressVariable 配置不存在)
    //   + WriteBytes 派生长度族 (PDULength/DataLength/DataBits/DataLen/RegisterCount/ByteCount)
    //   适用对象: 写标签 (direction=write) 与声明了写能力的读写标签
    //   (writeOperation / writeBytesOperation 任一非空)
        // 读标签同样放行 outputs(derivedLength) 派生名 — 协议族变量
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
                // 此处不再无条件插入 "StartAddress" —
        //   协议族地址名 (StartAddress/RegisterCount/...) 一律由上方 protocol.outputs /
        //   op.outputs 的 derivedLength 声明动态收集, 不再假定任何具体名 (契约名收敛为 2 个).
                // 派生长度由 JSON 声明 (source=auto strategy=derivedLength) — 收集声明名放行.
                //   PDULength 仅当声明为 derivedLength 才放行 (无注册表兜底).
                //   derivedLength 只存在于 outputs 段 (inputs 使用 derivedLength 会被校验拒绝).
        if (!isWriteTag && !tag.writeOperation.empty()) {
            std::unordered_map<std::string, Core::OperationConfig>::const_iterator wit2 =
                proto->operations.find(tag.writeOperation);
            if (wit2 != proto->operations.end())
                for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                        wit2->second.outputs.begin(); it != wit2->second.outputs.end(); ++it)
                    if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
        }
                // 变长写模板的派生名同样放行 (如 RegisterCount = {WriteValue:len} / 2)
        if (!isWriteTag && !tag.writeBytesOperation.empty()) {
            std::unordered_map<std::string, Core::OperationConfig>::const_iterator wbit2 =
                proto->operations.find(tag.writeBytesOperation);
            if (wbit2 != proto->operations.end())
                for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                        wbit2->second.outputs.begin(); it != wbit2->second.outputs.end(); ++it)
                    if (it->second.isDerivedLength()) runtimeVars.insert(it->first);
        }
    }
        // 标签级 variables 若声明了 outputs 派生名, 警告将覆盖派生值
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
            if (!Core::TemplateGrammar::SplitPlaceholder(t[i], parts)) continue;
            const std::string& varName = parts[0];
            const bool isRaw = (parts.size() == 2 && IsRawSpec(parts[1]));
            if (isRaw) {
                // {Name:raw} 变长载荷: 唯一合法来源是运行时注入的写值变量
                //   (WriteService 把 payload 以 writeVariable 为键注入 rawVars;
                                //   tag/device 级 variableBytesHex 不存在 — 声明值从不进入帧)
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
        // 写模板占位符校验 — 变量域 = variables ∪ writeVariables (同名键覆盖);
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

// ── 设备级模板变量 (devices[].variables) ──────────────────────

/// 校验设备级变量名能否在协议中落脚。
/// 作用域: 协议 inputs/outputs + 各操作 inputs/outputs + 握手会话变量。
/// 未声明的键在解析期会被折入 tag.variables 但无人引用 —— 拼写错误
/// (如 DA1 -> DA01) 在运行期只表现为"改了配置却没生效", 因此加载期直接报错。
static void ValidateDeviceVariables(const Core::DeviceConfig& dev,
                                    const Core::ProtocolConfig& proto,
                                    ValidationResult& r) {
    if (dev.variables.empty()) return;
    const std::string ctx = dev.id.empty() ? std::string("(匿名设备)") : dev.id;

    std::set<std::string> declared;
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
            proto.inputs.begin(); it != proto.inputs.end(); ++it)
        declared.insert(it->first);
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
            proto.outputs.begin(); it != proto.outputs.end(); ++it)
        declared.insert(it->first);
    for (std::unordered_map<std::string, Core::OperationConfig>::const_iterator op =
            proto.operations.begin(); op != proto.operations.end(); ++op) {
        for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                op->second.inputs.begin(); it != op->second.inputs.end(); ++it)
            declared.insert(it->first);
        for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                op->second.outputs.begin(); it != op->second.outputs.end(); ++it)
            declared.insert(it->first);
    }
    for (size_t i = 0; i < proto.handshake.size(); ++i) {
        if (!proto.handshake[i].sessionVariable.empty())
            declared.insert(proto.handshake[i].sessionVariable);
    }

    for (std::map<std::string, uint32_t>::const_iterator it = dev.variables.begin();
            it != dev.variables.end(); ++it) {
        const std::string& name = it->first;
        if (Core::IsFrameReservedName(name)) {
            r.addError("设备 " + ctx + " variables 使用了保留名 \"" + name
                       + "\" (成帧原语名, 不可作为模板变量)");
            continue;
        }
        if (declared.find(name) == declared.end()) {
            std::string names;
            int shown = 0;
            for (std::set<std::string>::const_iterator d = declared.begin();
                    d != declared.end(); ++d) {
                if (shown == 12) { names += ", ..."; break; }
                if (shown > 0) names += ", ";
                names += *d;
                ++shown;
            }
            r.addError("设备 " + ctx + " variables 含未声明变量 \"" + name
                       + "\" (协议 " + proto.protocolName
                       + " 的 inputs/outputs/握手会话变量中均无此名, 拼写错误?); 可用: "
                       + names);
            continue;
        }
        // 显式值优先于 auto 策略 (引擎语义, 可用于钉住某次取值), 但通常是误配
        const Core::VariableConfig* vc = 0;
        std::unordered_map<std::string, Core::VariableConfig>::const_iterator pit =
            proto.inputs.find(name);
        if (pit != proto.inputs.end()) vc = &pit->second;
        else {
            std::unordered_map<std::string, Core::VariableConfig>::const_iterator oit =
                proto.outputs.find(name);
            if (oit != proto.outputs.end()) vc = &oit->second;
        }
        if (vc != 0 && vc->isAuto()) {
            r.addWarning("设备 " + ctx + " variables 声明了 auto 变量 \"" + name
                         + "\" (" + (vc->isDerivedLength() ? std::string("derivedLength") : vc->strategy)
                         + "); 显式值将覆盖自动计算, 如非有意请删除该键");
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
        // 设备级模板变量须能在协议中落脚 (拼写错误在运行期不可见)
        std::unordered_map<std::string, const Core::ProtocolConfig*>::const_iterator dp =
            protoMap.find(root.devices[i].protocol);
        if (dp != protoMap.end()) {
            ValidateDeviceVariables(root.devices[i], *dp->second, r);
        }
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
        // 写标签 readBackTag 引用校验: 须指向存在的读标签
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

}} // namespace MyProt::Service

// src/Service/src/ValidationIssue.cpp
// 校验消息 → 结构化问题 (规则表分类)。实现说明见 ValidationIssue.hpp。
//
// 规则表 (kRules) 的字段语义:
//   id            规则标识 (前端按此决定跳转目标与图标)
//   scope         限定校验域 ("" = 任意域), 用于同一句话在不同域下归入不同规则
//   needles       '|' 分隔的子串, 命中任一即归入本规则 (顺序敏感: 具体在前)
//   subjectWords  '|' 分隔的主体关键词, 依次尝试; 取关键词后首个"词"作为 subject
//   fieldPattern  含 {subject} 占位; subject 为空时整体置空 (避免 "operations..x" 之类脏值)
//
// 新增规则 = 表里加一行; 不需改动校验器与其它任何文件。

#include "MyProt/Service/ValidationIssue.hpp"

#include <cstring>

namespace MyProt { namespace Service {

namespace {

const char* const kWarnPrefix = "[WARN] ";
const char* const kUnclassified = "unclassified";

/// '|' 分隔串中是否存在任一子串
bool MatchesAny(const std::string& s, const char* needles) {
    const std::string joined(needles ? needles : "");
    std::size_t pos = 0;
    while (pos <= joined.size()) {
        const std::size_t bar = joined.find('|', pos);
        const std::string one = joined.substr(
            pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!one.empty() && s.find(one) != std::string::npos) return true;
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    return false;
}

/// 按 '|' 顺序尝试关键词, 取"关键词之后的首个词" (以空格/':'/'"' 收尾)。
/// 取首个非空结果 — 例: "设备 X 引用不存在的协议" → "X"。
std::string SubjectFrom(const std::string& msg, const char* words) {
    const std::string joined(words ? words : "");
    std::size_t pos = 0;
    while (pos <= joined.size()) {
        const std::size_t bar = joined.find('|', pos);
        const std::string kw = joined.substr(
            pos, bar == std::string::npos ? std::string::npos : bar - pos);
        if (!kw.empty()) {
            const std::size_t p = msg.find(kw);
            if (p != std::string::npos) {
                const std::size_t b = p + kw.size();
                std::size_t e = b;
                while (e < msg.size()
                       && msg[e] != ' ' && msg[e] != ':' && msg[e] != '"') {
                    ++e;
                }
                if (e > b) return msg.substr(b, e - b);
            }
        }
        if (bar == std::string::npos) break;
        pos = bar + 1;
    }
    return std::string();
}

/// 用 subject 展开 fieldPattern 中的 {subject}。
/// - 常量模式 (不含 {subject}, 如 "schemaVersion") → 原样返回, 与 subject 无关
/// - 含 {subject} 但 subject 为空 → 返回空串 (避免 "operations..x" 之类脏值)
std::string FieldFrom(const char* pattern, const std::string& subject) {
    if (!pattern || pattern[0] == '\0') return std::string();
    const std::string pat(pattern);
    const std::string token("{subject}");
    const std::size_t p = pat.find(token);
    if (p == std::string::npos) return pat;
    if (subject.empty()) return std::string();
    return pat.substr(0, p) + subject + pat.substr(p + token.size());
}

struct Rule {
    const char* id;
    const char* scope;          // "" = 任意
    const char* needles;
    const char* subjectWords;   // "" = 不提取主体
    const char* fieldPattern;
};

/// 规则表 — 顺序敏感 (具体规则在前)。
/// 第一批 (保存被拒的主因): 帧长一致性 / 派生长度 expr / 派生长度偏移自检 /
///   设备→协议 / 标签→设备 / 标签→操作 / 模板文法 / 版本门禁 / 名称重复
/// 第二批 (参数与字段类, 覆盖实测产出的其余消息): 成帧参数 (Silence / LengthField /
///   framing.type) / 传输参数 (transport.type / host / serial) / TLS /
///   操作与协议命名 / 标签字段 (finalType / bitOffset / direction)
/// 注: [webApi] 段消息属全局 schema 域, 不经 protocols|tags 域校验, 故**有意不建规则**
///   (规则表宁缺勿假: 建了也永远不会命中)。
const Rule kRules[] = {
    // ── 成帧类 (ADR-0012 §2 试算, FrameConsistencyCheck.cpp) ──
    { "frame.length_consistency", "protocol",
      "帧长一致性失败|短于长度槽位要求",
      "操作 ", "operations.{subject}.requestTemplate" },
    { "frame.derived_expr", "protocol",
      "expr 求值失败|试渲染失败",
      "操作 ", "operations.{subject}.outputs" },
    { "frame.length_offset", "protocol",
      "偏移自检失败",
      "操作 ", "operations.{subject}.outputs" },

    // ── 引用完整性类 (规则 9/12, 跨文件) ──
    { "ref.device_protocol", "",
      "引用不存在的协议|所属设备的协议不存在",
      "设备 |标签 ", "devices.{subject}.protocol" },
    { "ref.tag_device", "tags",
      "引用不存在的设备",
      "标签 ", "tags.{subject}.deviceId" },
    { "ref.tag_operation", "tags",
      "操作引用不存在|写操作引用不存在|变长写操作引用不存在|指向非写操作|指向非读操作|readBackTag",
      "标签 ", "tags.{subject}.operation" },

    // ── 模板文法类 (规则 6 / v1.16 / v1.13 移除的三段文法) ──
    { "template.grammar", "protocol",
      "模板占位符|模板 token|空模板行|模板行全为空白|:auto: 令牌|须独占模板元素",
      "操作 ", "operations.{subject}.requestTemplate" },

    // ── 门禁 / 命名类 ──
    { "schema.version_gate", "",
      "配置代际过旧|配置代际过新",
      "", "schemaVersion" },
    { "name.duplicate", "",
      "全局重复",
      "全局重复: ", "" },

    // ── 第二批: 成帧参数类 (规则2-5; Silence 与 LengthField 都含 "maxFrameSize", 前者在前) ──
    { "frame.silence_params", "protocol",
      "Silence frameGapUs 必须 ≥ 1|Silence maxFrameSize 必须 > 0",
      "", "framing" },
    { "frame.length_field_params", "protocol",
      "Fixed fixedLength 必须 > 0|headerLength 必须 ≥|lengthFieldOffset 必须 ≥ 0|lengthFieldLength 取值须为|maxFrameSize (",
      "", "framing" },
    { "frame.framing_type", "protocol",
      "framing.type 判别值越界|未知 framing.type|framing.type=\"Message\"",
      "", "framing.type" },

    // ── 第二批: 传输 / 通道参数类 (规则2/10) ──
    { "transport.type", "protocol",
      "transport.type 判别值越界|未知 transport.type",
      "", "transport.type" },
    { "transport.host", "protocol",
      "connection.host 不能为空",
      "设备 ", "devices.{subject}.connection.host" },
    { "transport.serial_device", "protocol",
      "Serial 设备 |Serial baudRate 必须 > 0|charTimeUs=0 时协议级 baudRate",
      "设备 ", "devices.{subject}.transport" },
    { "tls.ca_file", "protocol",
      "TLS caFile 文件不存在",
      "", "transport.tls.caFile" },
    { "tls.not_implemented", "protocol",
      "TLS 传输的运行期通道尚未实现",
      "", "transport.tls" },

    // ── 第二批: 操作 / 协议命名类 (规则1/6) ──
    { "operation.empty", "protocol",
      "operations 不能为空",
      "", "operations" },
    { "name.protocol_empty", "protocol",
      "protocolName 为空",
      "", "protocolName" },

    // ── 第二批: 标签字段类 (tags 域; 规则13) ──
    { "tag.final_type", "tags",
      "finalType 取值非法|finalType=Bool 未声明 bitOffset",
      "标签 ", "tags.{subject}.finalType" },
    { "tag.bit_offset", "tags",
      "bitOffset 必须 0-7|Bool 位标签 ByteCount",
      "标签 ", "tags.{subject}.bitOffset" },
    { "tag.direction", "tags",
      "direction 取值非法",
      "标签 ", "tags.{subject}.direction" },
};

} // namespace

std::vector<ValidationIssue> ClassifyValidationItems(
    const std::vector<std::string>& items, const std::string& scope) {

    std::vector<ValidationIssue> out;
    out.reserve(items.size());

    const std::size_t ruleCount = sizeof(kRules) / sizeof(kRules[0]);

    for (std::size_t i = 0; i < items.size(); ++i) {
        ValidationIssue issue;
        const std::string& raw = items[i];
        const bool isWarn = raw.compare(0, std::strlen(kWarnPrefix), kWarnPrefix) == 0;
        issue.severity = isWarn ? "warning" : "error";
        issue.message = isWarn ? raw.substr(std::strlen(kWarnPrefix)) : raw;
        issue.scope = scope;
        issue.ruleId = kUnclassified;

        for (std::size_t k = 0; k < ruleCount; ++k) {
            const Rule& rule = kRules[k];
            if (rule.scope[0] != '\0' && scope != rule.scope) continue;
            if (!MatchesAny(issue.message, rule.needles)) continue;
            issue.ruleId = rule.id;
            issue.subject = SubjectFrom(issue.message, rule.subjectWords);
            issue.field = FieldFrom(rule.fieldPattern, issue.subject);
            break;
        }

        out.push_back(issue);
    }
    return out;
}

}} // namespace MyProt::Service

// tests/Service.Tests/ValidationIssueTests.cpp
// 校验消息 → 结构化问题 的单元测试。
// 消息样本均取自校验器实际产出文本 (ConfigDeepValidator.cpp / FrameConsistencyCheck.cpp),
// 避免"测试自己编格式、实现按测试编"的自证循环。
//
// 被测 TU 直接编译 (见 vcxproj), 不链接 MyProt.Service.lib — 该 TU 只依赖 <string>/<vector>。

#include "MyProt/Service/ValidationIssue.hpp"
#include "MiniTest.hpp"

#include <string>
#include <vector>

using MyProt::Service::ValidationIssue;
using MyProt::Service::ClassifyValidationItems;

namespace {

typedef std::vector<std::string> Items;

ValidationIssue One(const std::string& msg, const std::string& scope = "protocol") {
    Items items(1, msg);
    const std::vector<ValidationIssue> out = ClassifyValidationItems(items, scope);
    if (out.empty()) return ValidationIssue();
    return out[0];
}

} // namespace

// ── 规则 1: 帧长一致性 ──
TEST(ValidationIssueTest, FrameLengthConsistency) {
    const ValidationIssue i = One(
        "操作 WriteMultipleRegisters 帧长一致性失败: 长度槽位值 10 ≠ 期望 9 "
        "(渲染总长 16B, 载荷 3B); 模板固定段与派生长度/成帧配置不一致 (ADR-0012 §2)");
    EXPECT_EQ(i.ruleId, "frame.length_consistency");
    EXPECT_EQ(i.severity, "error");
    EXPECT_EQ(i.scope, "protocol");
    EXPECT_EQ(i.subject, "WriteMultipleRegisters");
    EXPECT_EQ(i.field, "operations.WriteMultipleRegisters.requestTemplate");
}

TEST(ValidationIssueTest, FrameShortThanLengthSlot) {
    const ValidationIssue i = One(
        "操作 WriteRegisters 渲染帧 (4B) 短于长度槽位要求 (6B) (ADR-0012 §2)");
    EXPECT_EQ(i.ruleId, "frame.length_consistency");
    EXPECT_EQ(i.subject, "WriteRegisters");
}

// ── 规则 2: 派生长度 expr 求值失败 ──
TEST(ValidationIssueTest, FrameDerivedExpr) {
    const ValidationIssue i = One(
        "操作 WriteRegisters 派生长度 \"ByteCount\" expr 求值失败 "
        "(语法错误或引用了未知变量/原语), 渲染时该字段将缺失 (ADR-0012 §2)");
    EXPECT_EQ(i.ruleId, "frame.derived_expr");
    EXPECT_EQ(i.subject, "WriteRegisters");
    EXPECT_EQ(i.field, "operations.WriteRegisters.outputs");
}

// ── 规则 3: 派生长度偏移自检 ──
TEST(ValidationIssueTest, FrameLengthOffsetSelfCheck) {
    const ValidationIssue i = One(
        "操作 WriteBytes 派生长度 \"ByteCount\" 偏移自检失败: 声明偏移 4 ≠ 模板期望 3 "
        "(成帧长度槽位 offset=4 length=2 includesHeader=true; 改动模板固定字节后需同步该派生长度偏移)");
    EXPECT_EQ(i.ruleId, "frame.length_offset");
    EXPECT_EQ(i.subject, "WriteBytes");
}

// ── 规则 4: 设备 → 协议引用 (tags 域, 规则9) ──
TEST(ValidationIssueTest, RefDeviceProtocolInTagsScope) {
    const ValidationIssue i = One(
        "设备 PLC-001 引用不存在的协议: \"modbus-tcp\" (规则9)", "tags");
    EXPECT_EQ(i.ruleId, "ref.device_protocol");
    EXPECT_EQ(i.scope, "tags");
    EXPECT_EQ(i.subject, "PLC-001");
    EXPECT_EQ(i.field, "devices.PLC-001.protocol");
}

// ── 规则 4 变体: 标签所属设备的协议不存在 (主体关键词须落在 "标签") ──
TEST(ValidationIssueTest, RefTagOwnerProtocolPrefersTagKeyword) {
    const ValidationIssue i = One(
        "标签 PLC-001.Coil0 所属设备的协议不存在: \"modbus-tcp\" (规则12)", "tags");
    EXPECT_EQ(i.ruleId, "ref.device_protocol");
    EXPECT_EQ(i.subject, "PLC-001.Coil0");
}

// ── 规则 5: 标签 → 设备引用 ──
TEST(ValidationIssueTest, RefTagDevice) {
    const ValidationIssue i = One(
        "标签 PLC-001.Coil0 引用不存在的设备: \"PLC-002\" (规则12)", "tags");
    EXPECT_EQ(i.ruleId, "ref.tag_device");
    EXPECT_EQ(i.subject, "PLC-001.Coil0");
    EXPECT_EQ(i.field, "tags.PLC-001.Coil0.deviceId");
}

// ── 规则 6: 标签 → 操作引用 ──
TEST(ValidationIssueTest, RefTagOperation) {
    const ValidationIssue i = One(
        "标签 PLC-001.HR0 操作引用不存在: \"ReadHoldingRegisters\" (规则12)", "tags");
    EXPECT_EQ(i.ruleId, "ref.tag_operation");
    EXPECT_EQ(i.subject, "PLC-001.HR0");
    EXPECT_EQ(i.field, "tags.PLC-001.HR0.operation");
}

// ── 规则 7: 模板文法 ──
TEST(ValidationIssueTest, TemplateGrammar) {
    const ValidationIssue i = One(
        "操作 WriteRegisters 模板占位符 \"Frame\" 为已移除的三段文法; 请改用 {Name:Xn} (规则6)");
    EXPECT_EQ(i.ruleId, "template.grammar");
    EXPECT_EQ(i.subject, "WriteRegisters");
}

// ── 规则 8: 版本门禁 ──
TEST(ValidationIssueTest, SchemaVersionGate) {
    const ValidationIssue i = One(
        "配置代际过旧: schemaVersion=1, 但当期支持 2 (规则0)");
    EXPECT_EQ(i.ruleId, "schema.version_gate");
    EXPECT_EQ(i.field, "schemaVersion");
    EXPECT_TRUE(i.subject.empty());
}

// ── 规则 9: 名称全局重复 (无引号, 取 ": " 之后首个词) ──
TEST(ValidationIssueTest, NameDuplicate) {
    const ValidationIssue i = One("protocolName 全局重复: modbus-tcp (规则1)", "tags");
    EXPECT_EQ(i.ruleId, "name.duplicate");
    EXPECT_EQ(i.subject, "modbus-tcp");
}

// ── 兜底: 未归类条目仍原样返回 (只增信息, 不丢信息) ──
TEST(ValidationIssueTest, UnclassifiedKeepsMessage) {
    // 样本取 [webApi] 段真实消息: 属全局 schema 域, 规则表有意不建 (见 webApi 边界用例)
    const ValidationIssue i = One("[webApi] bindAddress 不是合法 IPv4 地址: \"999.1.1.1\"");
    EXPECT_EQ(i.ruleId, "unclassified");
    EXPECT_EQ(i.message, "[webApi] bindAddress 不是合法 IPv4 地址: \"999.1.1.1\"");
    EXPECT_EQ(i.severity, "error");
    EXPECT_TRUE(i.subject.empty());
    EXPECT_TRUE(i.field.empty());
}

// ── Warning 前缀: severity=warning 且 message 剥离前缀 ──
TEST(ValidationIssueTest, WarningPrefixStripped) {
    const ValidationIssue i = One(
        "[WARN] 标签 PLC-001.Coil0 写标签 (direction=write) 无需 writeOperation, 将被忽略 (规则13)",
        "tags");
    EXPECT_EQ(i.severity, "warning");
    EXPECT_EQ(i.message,
              "标签 PLC-001.Coil0 写标签 (direction=write) 无需 writeOperation, 将被忽略 (规则13)");
}

// ── 顺序与条目数不变 (同一域内多条目; 真实调用即单域多条) ──
TEST(ValidationIssueTest, OrderAndCountPreserved) {
    Items items;
    items.push_back("操作 A 帧长一致性失败: 长度槽位值 1 ≠ 期望 2");
    items.push_back("[webApi] rateLimitBurst 必须 ≥ rateLimitRps");   // 规则表外 → unclassified
    items.push_back("操作 B 派生长度 \"ByteCount\" expr 求值失败 (语法错误或引用了未知变量/原语)");

    const std::vector<ValidationIssue> out = ClassifyValidationItems(items, "protocol");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].ruleId, "frame.length_consistency");
    EXPECT_EQ(out[1].ruleId, "unclassified");
    EXPECT_EQ(out[2].ruleId, "frame.derived_expr");
    EXPECT_EQ(out[0].message, items[0]);
    EXPECT_EQ(out[2].message, items[2]);
}

// ── tags 域内多条目 (设备→协议 / 标签→设备 / 标签→操作) 顺序保持 ──
TEST(ValidationIssueTest, TagScopeOrderPreserved) {
    Items items;
    items.push_back("设备 D1 引用不存在的协议: \"p-x\" (规则9)");
    items.push_back("标签 T1 引用不存在的设备: \"D9\" (规则12)");
    items.push_back("标签 T2 操作引用不存在: \"Op9\" (规则12)");

    const std::vector<ValidationIssue> out = ClassifyValidationItems(items, "tags");
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].ruleId, "ref.device_protocol");
    EXPECT_EQ(out[1].ruleId, "ref.tag_device");
    EXPECT_EQ(out[2].ruleId, "ref.tag_operation");
    EXPECT_EQ(out[0].subject, "D1");
    EXPECT_EQ(out[1].subject, "T1");
    EXPECT_EQ(out[2].subject, "T2");
}

// ── 域隔离: protocol 域规则不误匹配 tags 专有规则 ──
TEST(ValidationIssueTest, ScopeIsolation) {
    // "ref.tag_device" 规则限定 tags 域; 同文本在 protocol 域下不应归类
    const ValidationIssue i = One("标签 T1 引用不存在的设备: \"D9\" (规则12)", "protocol");
    EXPECT_EQ(i.ruleId, "unclassified");
}

// ══════════════════════════════════════════════════════════════════
// 第二批规则 (参数与字段类) — 样本同样取自 ConfigDeepValidator.cpp 原文
// ══════════════════════════════════════════════════════════════════

// ── 成帧: framing.type 越界 / CAN 预留 ──
TEST(ValidationIssueTest, FrameFramingType) {
    const ValidationIssue i = One("framing.type=\"Message\" 为 CAN 预留, v1 不支持 (ADR-0002)");
    EXPECT_EQ(i.ruleId, "frame.framing_type");
    EXPECT_EQ(i.field, "framing.type");

    const ValidationIssue j = One("framing.type 判别值越界 (规则2)");
    EXPECT_EQ(j.ruleId, "frame.framing_type");
}

// ── 成帧: LengthField 参数 ──
TEST(ValidationIssueTest, FrameLengthFieldParams) {
    const ValidationIssue i = One("LengthField lengthFieldLength 取值须为 1/2/4, 实际 3 (规则3)");
    EXPECT_EQ(i.ruleId, "frame.length_field_params");
    EXPECT_EQ(i.field, "framing");

    const ValidationIssue j = One("Fixed fixedLength 必须 > 0 (规则4)");
    EXPECT_EQ(j.ruleId, "frame.length_field_params");
}

// ── 顺序敏感: Silence 消息同时含 "maxFrameSize", 必须归入 Silence 而非 LengthField ──
TEST(ValidationIssueTest, FrameSilenceWinsOverLengthField) {
    const ValidationIssue i = One("Silence maxFrameSize 必须 > 0 (规则5)");
    EXPECT_EQ(i.ruleId, "frame.silence_params");

    const ValidationIssue j = One("Silence frameGapUs 必须 ≥ 1 (静态静默阈值须显式配置; 规则5)");
    EXPECT_EQ(j.ruleId, "frame.silence_params");
}

// ── 传输: 判别值 / 主机名 / 串口 ──
TEST(ValidationIssueTest, TransportTypeAndHost) {
    const ValidationIssue t = One("transport.type 判别值越界 (规则2)");
    EXPECT_EQ(t.ruleId, "transport.type");
    EXPECT_EQ(t.field, "transport.type");

    const ValidationIssue h = One("Tcp/Tls 设备 D1 connection.host 不能为空 (规则10)");
    EXPECT_EQ(h.ruleId, "transport.host");
    EXPECT_EQ(h.subject, "D1");
    EXPECT_EQ(h.field, "devices.D1.connection.host");

    const ValidationIssue s = One("Serial 设备 D2 baudRate 必须 > 0 (规则10)");
    EXPECT_EQ(s.ruleId, "transport.serial_device");
    EXPECT_EQ(s.subject, "D2");
}

// ── TLS: caFile 缺失是 warning (不阻断保存) ──
TEST(ValidationIssueTest, TlsCaFileWarnsButNotBlocks) {
    const ValidationIssue i = One("[WARN] TLS caFile 文件不存在: certs/ca.pem (规则2)");
    EXPECT_EQ(i.ruleId, "tls.ca_file");
    EXPECT_EQ(i.severity, "warning");
    EXPECT_EQ(i.field, "transport.tls.caFile");

    const ValidationIssue j = One("[WARN] TLS 传输的运行期通道尚未实现, 设备将无法建立连接 (tls 暂缓决策)");
    EXPECT_EQ(j.ruleId, "tls.not_implemented");
}

// ── 操作与协议命名 ──
TEST(ValidationIssueTest, OperationAndProtocolNameGaps) {
    const ValidationIssue o = One("operations 不能为空 (规则6)");
    EXPECT_EQ(o.ruleId, "operation.empty");
    EXPECT_EQ(o.field, "operations");

    const ValidationIssue p = One("protocolName 为空 (规则1)");
    EXPECT_EQ(p.ruleId, "name.protocol_empty");
    EXPECT_EQ(p.field, "protocolName");
}

// ── 标签字段: finalType / bitOffset / direction ──
TEST(ValidationIssueTest, TagFieldRules) {
    const ValidationIssue d = One("标签 T1 direction 取值非法: \"up\" (规则13)", "tags");
    EXPECT_EQ(d.ruleId, "tag.direction");
    EXPECT_EQ(d.subject, "T1");
    EXPECT_EQ(d.field, "tags.T1.direction");

    const ValidationIssue f = One("标签 T1 finalType 取值非法: \"Float\" (规则13)", "tags");
    EXPECT_EQ(f.ruleId, "tag.final_type");
    EXPECT_EQ(f.field, "tags.T1.finalType");

    const ValidationIssue b = One("标签 T1 bitOffset 必须 0-7, 实际 9 (规则13)", "tags");
    EXPECT_EQ(b.ruleId, "tag.bit_offset");

    // 同一批规则的 warning 变体 (Bool 位标签宽度不一致)
    const ValidationIssue w = One("[WARN] 标签 T1 Bool 位标签 ByteCount=2 与 bitOffset 语义不符 (规则13)", "tags");
    EXPECT_EQ(w.ruleId, "tag.bit_offset");
    EXPECT_EQ(w.severity, "warning");
}

// ── 边界: webApi 段消息在 protocols|tags 域下有意保持 unclassified (规则表不建死规则) ──
TEST(ValidationIssueTest, WebApiScopeDeliberatelyUnclassified) {
    const ValidationIssue i = One("[WARN] [webApi] rateLimitRps 必须 ≥ 1");
    EXPECT_EQ(i.ruleId, "unclassified");
    EXPECT_TRUE(i.field.empty());

    const ValidationIssue j = One("[webApi] certFile 与 keyFile 须同时为空或同时非空 (仅配其一)", "tags");
    EXPECT_EQ(j.ruleId, "unclassified");
}

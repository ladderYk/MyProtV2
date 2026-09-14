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

// ── 规则 3: 派生长度偏移自检 (v1.13) ──
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
    const ValidationIssue i = One("protocolName 为空 (规则1)");
    EXPECT_EQ(i.ruleId, "unclassified");
    EXPECT_EQ(i.message, "protocolName 为空 (规则1)");
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
    items.push_back("protocolName 为空 (规则1)");
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

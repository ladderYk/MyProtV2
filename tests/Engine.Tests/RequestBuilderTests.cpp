// tests/Engine.Tests/RequestBuilderTests.cpp
// RequestBuilder 单元测试 — 模板展开 (十六进制字面量 + 变量占位符宽度) /
// 三段变量合并优先级 / 模板布局扫描 (ADR-0012 派生长度求值的基础) / auto 自增策略。

#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"
#include "MiniTest.hpp"

#include <unordered_map>
#include <vector>

using namespace MyProt::Core;
using namespace MyProt::Engine;

// ── 模板展开: hex 字面量 (每 2 字符 = 1 字节) + {Name:X4} 大端 4 位十六进制 ──
TEST(RequestBuilderTest, HexLiteralAndFixedWidthVariable) {
    OperationConfig op;
    op.requestTemplate.push_back("01");                  // 从站地址
    op.requestTemplate.push_back("03");                  // 功能码
    op.requestTemplate.push_back("{StartAddress:X4}");   // 起始地址 (2 字节)
    op.requestTemplate.push_back("{RegisterCount:X4}");  // 寄存器数 (2 字节)

    std::unordered_map<std::string, uint32_t> vars;
    vars["StartAddress"] = 0x006Bu;
    vars["RegisterCount"] = 2u;

    AutoComputeProvider autoProvider;
    RequestBuilder builder;
    const Expected<Bytes> built = builder.Build(op, vars, std::unordered_map<std::string, std::string>(), autoProvider);
    ASSERT_TRUE(built.has_value());

    const Bytes& frame = built.value();
    ASSERT_EQ(frame.size(), 6u);
    EXPECT_EQ(frame[0], 0x01);
    EXPECT_EQ(frame[1], 0x03);
    EXPECT_EQ(frame[2], 0x00);   // 大端高位
    EXPECT_EQ(frame[3], 0x6B);
    EXPECT_EQ(frame[4], 0x00);
    EXPECT_EQ(frame[5], 0x02);
}

// ── 变量缺失: 必须报错, 不能渲染成 0 后发出去 ──
TEST(RequestBuilderTest, MissingVariableIsError) {
    OperationConfig op;
    op.requestTemplate.push_back("{NotProvided:X4}");

    std::unordered_map<std::string, uint32_t> vars;   // 故意空
    AutoComputeProvider autoProvider;
    RequestBuilder builder;
    const Expected<Bytes> built = builder.Build(op, vars, std::unordered_map<std::string, std::string>(), autoProvider);
    EXPECT_FALSE(built.has_value());
}

// ── 三段合并优先级: 标签 > op static > 协议 default ──
TEST(RequestBuilderTest, MergeVariablesPriority) {
    std::unordered_map<std::string, uint32_t> proto;
    proto["A"] = 1u;   // 仅协议级
    proto["B"] = 1u;   // protocol + op
    proto["C"] = 1u;   // 三级都有

    std::unordered_map<std::string, uint32_t> opLevel;
    opLevel["B"] = 2u;
    opLevel["C"] = 2u;

    std::unordered_map<std::string, uint32_t> tagLevel;
    tagLevel["C"] = 3u;

    const std::unordered_map<std::string, uint32_t> merged =
        RequestBuilder::MergeVariables(proto, opLevel, tagLevel);

    EXPECT_EQ(merged.at("A"), 1u);   // 只有协议级 → 取协议级
    EXPECT_EQ(merged.at("B"), 2u);   // op 覆盖协议
    EXPECT_EQ(merged.at("C"), 3u);   // 标签覆盖 op
}

// ── 模板布局: 占位符宽度 / 首现偏移 / 固定段总宽 (ADR-0012 派生长度的输入) ──
TEST(RequestBuilderTest, TemplateLayoutWidthsOffsetsFixedTotal) {
    std::vector<std::string> tpl;
    tpl.push_back("01");            // 1 字节
    tpl.push_back("10");            // 1 字节
    tpl.push_back("{TID:X4}");      // 4 位 hex = 2 字节
    tpl.push_back("{Count:X2}");    // 2 位 hex = 1 字节

    const TemplateLayout layout = RequestBuilder::BuildTemplateLayout(tpl);

    EXPECT_FALSE(layout.hasUnknown);
    EXPECT_EQ(layout.widths.at("TID"), 2u);
    EXPECT_EQ(layout.widths.at("Count"), 1u);
    EXPECT_EQ(layout.offsets.at("TID"), 2u);      // 前两个 hex 元素各 1 字节
    EXPECT_EQ(layout.offsets.at("Count"), 4u);    // 1 + 1 + 2
    EXPECT_EQ(layout.fixedTotal, 5u);             // hex 1 + hex 1 + X4(2) + X2(1)
}

// ── auto 策略: 声明式自增 (modbus TransactionID 的语义) ──
TEST(RequestBuilderTest, AutoIncrementDeclaredStrategy) {
    AutoComputeProvider autoProvider;
    const bool declared = autoProvider.DeclareJson(
        "{\"TID\":{\"strategy\":\"autoIncrement\",\"params\":{\"seed\":1}}}");
    ASSERT_TRUE(declared);
    EXPECT_TRUE(autoProvider.IsDeclared("TID"));

    // 占位符写普通 {Name:Xn}; "是 auto" 由 DeclareJson 的声明决定 (E2E 同款写法)
    OperationConfig op;
    op.name = "AutoOp";
    op.kind = "read";
    op.requestTemplate.push_back("{TID:X4}");
    op.requestTemplate.push_back("03");

    std::unordered_map<std::string, uint32_t> vars;
    RequestBuilder builder;

    const Expected<Bytes> first = builder.Build(op, vars, std::unordered_map<std::string, std::string>(), autoProvider);
    const Expected<Bytes> second = builder.Build(op, vars, std::unordered_map<std::string, std::string>(), autoProvider);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_EQ(first.value().size(), 3u);
    ASSERT_EQ(second.value().size(), 3u);

    EXPECT_EQ(first.value()[1], 0x01);    // seed = 1
    EXPECT_EQ(second.value()[1], 0x02);   // 原子 +1
    EXPECT_EQ(first.value()[2], 0x03);    // 后续字面量不受影响
}

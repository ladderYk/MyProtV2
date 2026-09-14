// tests/Engine.Tests/ExpressionEvaluatorTests.cpp
// ExpressionEvaluator 单元测试 — resp[N] 字节引用 / 十六进制字面量 / 比较与逻辑运算 /
// C 优先级对齐 (|| < && < | < ^ < & < == < 移位 < 加减 < 乘除 < 一元; ADR-0003 KI-13)。
//
// 这些表达式是配置里 validCondition / dataLengthExpr / 派生长度 expr 的求值内核,
// 语义错了会静默产出错误的标签值, 所以在此按"最小可判定集合"固定住。

#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Engine/ExpressionEvaluator.hpp"
#include "MiniTest.hpp"

using namespace MyProt::Core;
using namespace MyProt::Engine;

namespace {

/// 样本帧: resp[0]=0xAA resp[1]=0x03 resp[2]=0x10 resp[3]=0x00
Bytes SampleFrame() {
    Bytes b;
    b.push_back(0xAA);
    b.push_back(0x03);
    b.push_back(0x10);
    b.push_back(0x00);
    return b;
}

} // namespace

// ── 条件: resp[N] 与十六进制字面量比较 (配置里最常见的 validCondition) ──
TEST(ExpressionEvaluatorTest, ConditionEqualsHexLiteral) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    ExpressionEvaluator ev;
    const Expected<bool> hit = ev.EvaluateCondition("resp[1] == 0x03", resp);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(hit.value());

    const Expected<bool> miss = ev.EvaluateCondition("resp[1] == 0x04", resp);
    ASSERT_TRUE(miss.has_value());
    EXPECT_FALSE(miss.value());
}

// ── 条件: 逻辑与/或 (&& 优先级高于 ||) ──
TEST(ExpressionEvaluatorTest, ConditionLogicalAndOr) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    ExpressionEvaluator ev;
    // both true
    const Expected<bool> a = ev.EvaluateCondition("resp[1] == 0x03 && resp[2] != 0x00", resp);
    ASSERT_TRUE(a.has_value());
    EXPECT_TRUE(a.value());

    // 左真右假 → && 结果假 (若优先级错成 (a&&b)||c 会误判)
    const Expected<bool> b = ev.EvaluateCondition("resp[1] == 0x03 && resp[2] == 0x00", resp);
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(b.value());

    // 任一侧真 → || 结果真
    const Expected<bool> c = ev.EvaluateCondition("resp[1] == 0x99 || resp[2] == 0x10", resp);
    ASSERT_TRUE(c.has_value());
    EXPECT_TRUE(c.value());
}

// ── 长度: 单字节引用 ──
TEST(ExpressionEvaluatorTest, LengthFromSingleByteRef) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    ExpressionEvaluator ev;
    const Expected<int> n = ev.EvaluateLength("resp[2]", resp);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(n.value(), 0x10);
}

// ── 长度: 算术优先级 (乘除先于加减) 与括号/移位组合 ──
TEST(ExpressionEvaluatorTest, LengthArithmeticPrecedence) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    ExpressionEvaluator ev;
    // 3 * 2 + 1 = 7 (若优先级反了会得 9)
    const Expected<int> mulFirst = ev.EvaluateLength("resp[1] * 2 + 1", resp);
    ASSERT_TRUE(mulFirst.has_value());
    EXPECT_EQ(mulFirst.value(), 7);

    // (0x03 << 8) | 0x10 = 0x0310 (大端拼字)
    const Expected<int> bigEndianWord = ev.EvaluateLength("(resp[1] << 8) | resp[2]", resp);
    ASSERT_TRUE(bigEndianWord.has_value());
    EXPECT_EQ(bigEndianWord.value(), 0x0310);
}

// ── 非法表达式必须是"错误"而不是抛异常 / 静默 0 ──
TEST(ExpressionEvaluatorTest, InvalidExpressionReturnsError) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    ExpressionEvaluator ev;
    bool threw = false;
    bool gotValue = true;
    try {
        const Expected<bool> bad = ev.EvaluateCondition("bogus[0] == 1", resp);
        gotValue = bad.has_value();
    } catch (...) {
        threw = true;
    }
    EXPECT_FALSE(threw);
    EXPECT_FALSE(gotValue);
}

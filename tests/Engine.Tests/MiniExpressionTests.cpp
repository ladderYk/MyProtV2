// tests/Engine.Tests/MiniExpressionTests.cpp
// MiniExpression 单元测试 — 统一表达式引擎 (原 ExpressionEvaluator 职责已并入)。
// 覆盖: resp[N] 字节引用 / 十六进制字面量 / 比较与逻辑运算 / C 优先级 / 移位 / 错误传播。
//
// 这些表达式是配置里 validCondition / dataLengthExpr / 派生长度 expr 的求值内核,
// 语义错了会静默产出错误的标签值, 所以在此按"最小可判定集合"固定住。

#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Engine/MiniExpression.hpp"
#include "MiniTest.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace MyProt::Core;

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

/// resp[N] 解析: MiniExpression 以规范名 "resp[N]" 回调 (见 Node::Resp)
bool LookupResponseByte(ByteView response, const std::string& name, std::uint64_t& out) {
    const std::string prefix = "resp[";
    if (name.size() <= prefix.size() + 1
            || name.compare(0, prefix.size(), prefix) != 0
            || name[name.size() - 1] != ']') return false;
    const std::string idxText = name.substr(prefix.size(),
                                            name.size() - prefix.size() - 1);
    const long idx = std::strtol(idxText.c_str(), 0, 10);
    if (idx < 0 || static_cast<std::size_t>(idx) >= response.size) return false;
    out = response.data[static_cast<std::size_t>(idx)];
    return true;
}

/// 求值: 解析或求值失败返回 false (而非抛异常)
bool TryEval(const std::string& expr, ByteView response, std::uint64_t& out) {
    try {
        MyProt::Engine::MiniExpression::Parser parser(expr);
        parser.Parse();
        out = parser.Evaluate([response](const std::string& name, bool& found)
                                  -> std::uint64_t {
            std::uint64_t v = 0;
            if (LookupResponseByte(response, name, v)) { found = true; return v; }
            found = false;
            return 0;
        });
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

// ── 条件: resp[N] 与十六进制字面量比较 (配置里最常见的 validCondition) ──
TEST(MiniExpressionTest, ConditionEqualsHexLiteral) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    std::uint64_t hit = 0;
    ASSERT_TRUE(TryEval("resp[1] == 0x03", resp, hit));
    EXPECT_EQ(hit, 1u);

    std::uint64_t miss = 1;
    ASSERT_TRUE(TryEval("resp[1] == 0x04", resp, miss));
    EXPECT_EQ(miss, 0u);
}

// ── 条件: 逻辑与/或 (&& 优先级高于 ||) ──
TEST(MiniExpressionTest, ConditionLogicalAndOr) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    std::uint64_t a = 0;
    ASSERT_TRUE(TryEval("resp[1] == 0x03 && resp[2] != 0x00", resp, a));
    EXPECT_EQ(a, 1u);

    std::uint64_t b = 1;
    ASSERT_TRUE(TryEval("resp[1] == 0x03 && resp[2] == 0x00", resp, b));
    EXPECT_EQ(b, 0u);

    std::uint64_t c = 0;
    ASSERT_TRUE(TryEval("resp[1] == 0x99 || resp[2] == 0x10", resp, c));
    EXPECT_EQ(c, 1u);
}

// ── 长度: 单字节引用 ──
TEST(MiniExpressionTest, LengthFromSingleByteRef) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    std::uint64_t n = 0;
    ASSERT_TRUE(TryEval("resp[2]", resp, n));
    EXPECT_EQ(n, 0x10u);
}

// ── 长度: 算术优先级 (乘除先于加减) 与移位/位运算组合 ──
TEST(MiniExpressionTest, LengthArithmeticPrecedence) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    // 3 * 2 + 1 = 7 (若优先级反了会得 9)
    std::uint64_t mulFirst = 0;
    ASSERT_TRUE(TryEval("resp[1] * 2 + 1", resp, mulFirst));
    EXPECT_EQ(mulFirst, 7u);

    // (0x03 << 8) | 0x10 = 0x0310 (大端拼字)
    std::uint64_t bigEndianWord = 0;
    ASSERT_TRUE(TryEval("(resp[1] << 8) | resp[2]", resp, bigEndianWord));
    EXPECT_EQ(bigEndianWord, 0x0310u);
}

// ── 位运算优先级对齐 C: 位与/或低于算术与比较 ──
TEST(MiniExpressionTest, BitwisePrecedenceMatchesC) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    // C: 1 | 2 * 3 = 7 (旧 MiniExpression 会把 | 绑得更紧 → 9)
    std::uint64_t orMul = 0;
    ASSERT_TRUE(TryEval("1 | 2 * 3", resp, orMul));
    EXPECT_EQ(orMul, 7u);

    // C: resp[1] & 0xFF == 0x03 → resp[1] & (0xFF == 0x03) = 0
    std::uint64_t andEq = 1;
    ASSERT_TRUE(TryEval("resp[1] & 0xFF == 0x03", resp, andEq));
    EXPECT_EQ(andEq, 0u);
}

// ── 内建函数: hi8/lo8/hi16/lo16 取代 "% 256" / "/ 256 % 256" 手写算术 ──
TEST(MiniExpressionTest, BuiltinByteExtraction) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    // 字节提取: 0x1234 → hi8=0x12 lo8=0x34
    std::uint64_t hi = 0, lo = 0;
    ASSERT_TRUE(TryEval("hi8(0x1234)", resp, hi));
    ASSERT_TRUE(TryEval("lo8(0x1234)", resp, lo));
    EXPECT_EQ(hi, 0x12u);
    EXPECT_EQ(lo, 0x34u);

    // 字提取: 0x12345678 → hi16=0x1234 lo16=0x5678
    std::uint64_t hiw = 0, low = 0;
    ASSERT_TRUE(TryEval("hi16(0x12345678)", resp, hiw));
    ASSERT_TRUE(TryEval("lo16(0x12345678)", resp, low));
    EXPECT_EQ(hiw, 0x1234u);
    EXPECT_EQ(low, 0x5678u);
}

// ── 内建函数可与算术/响应字节组合 (等价于旧写法) ──
TEST(MiniExpressionTest, BuiltinComposesWithArithmetic) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    // hi8(0x1234 / 2) == 0x09; 旧写法 0x1234 / 2 / 256 % 256 同样得 0x09
    std::uint64_t a = 0, b = 0;
    ASSERT_TRUE(TryEval("hi8(0x1234 / 2)", resp, a));
    ASSERT_TRUE(TryEval("0x1234 / 2 / 256 % 256", resp, b));
    EXPECT_EQ(a, b);

    // 参数可以是响应字节表达式
    std::uint64_t c = 0;
    ASSERT_TRUE(TryEval("lo8((resp[1] << 8) | resp[2])", resp, c));
    EXPECT_EQ(c, 0x10u);

    // 允许函数名与 '(' 之间有空白
    std::uint64_t d = 0;
    ASSERT_TRUE(TryEval("hi8 (0x1234)", resp, d));
    EXPECT_EQ(d, 0x12u);
}

// ── 未知函数 / 参数个数错误必须在解析期失败 (不能运行期才静默) ──
TEST(MiniExpressionTest, BuiltinArityAndUnknownFunctionRejected) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    std::uint64_t unused = 0;
    EXPECT_FALSE(TryEval("hi9(1)", resp, unused));       // 未知函数名
    EXPECT_FALSE(TryEval("hi8(1, 2)", resp, unused));    // 多参数
    EXPECT_FALSE(TryEval("hi8(1", resp, unused));        // 缺右括号
    EXPECT_FALSE(TryEval("hi8()", resp, unused));        // 空参数
}

// ── 非法表达式必须是"错误"而不是抛异常 / 静默 0 ──
TEST(MiniExpressionTest, InvalidExpressionReturnsError) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    std::uint64_t unused = 0;
    EXPECT_FALSE(TryEval("bogus[0] == 1", resp, unused));
    EXPECT_FALSE(TryEval("resp[1] == 0x03 &&", resp, unused));
    EXPECT_FALSE(TryEval("", resp, unused));
}

// ═══════════════════════════════════════════════════════════════════
// 优先级矩阵 (KI-13)
//
// 递归下降的层序即语义契约, 从低到高共 11 级 (与 C 对齐):
//   || → && → | → ^ → & → ==/!= → >/</>=/<= → <</>> → +/- → *//% → 一元
// 每行矩阵只改一个相邻层级, 让"层序错误"直接指向具体分支
// (例如 relational 层被漏掉时, `1 == 2 > 1` 会返回 0 而不是 1)。
// ═══════════════════════════════════════════════════════════════════

namespace {

struct PrecedenceCase {
    const char* expr;
    std::uint64_t expected;
};

/// 表驱动求值: 失败时打印表达式 (MiniTest 无 SCOPED_TRACE)
void ExpectExpr(ByteView resp, const char* expr, std::uint64_t expected) {
    std::uint64_t out = 0;
    const bool ok = TryEval(expr, resp, out);
    if (!ok || out != expected) {
        std::printf("      expr: %s -> %s%llu (期望 %llu)\n",
                    expr, ok ? "" : "求值失败 ",
                    static_cast<unsigned long long>(out),
                    static_cast<unsigned long long>(expected));
    }
    EXPECT_TRUE(ok);
    if (ok) EXPECT_EQ(out, expected);
}

void RunMatrix(ByteView resp, const PrecedenceCase* cases, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        ExpectExpr(resp, cases[i].expr, cases[i].expected);
    }
}

} // namespace

// ── 逐层比较: 低优先级运算符晚结合, 高优先级先算 ──
TEST(MiniExpressionTest, PrecedenceMatrixAllLevels) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    const PrecedenceCase cases[] = {
        // || 低于 &&
        { "1 || 0 && 0", 1u },
        // && 低于 | (1 && (0|2) = 1; 若反了会得 (1&&0)|2 = 2)
        { "1 && 0 | 2", 1u },
        // | 低于 ^ ((2^3)=1 → 1|1 = 1; 若反了 (1|2)^3 = 0)
        { "1 | 2 ^ 3", 1u },
        // ^ 低于 & ((1&1)=1 → 3^1 = 2; 若反了 (3^1)&1 = 0)
        { "3 ^ 1 & 1", 2u },
        // & 低于 == ((1==0)=0 → 2&0 = 0; 若反了 (2&1)==0 = 1)
        { "2 & 1 == 0", 0u },
        // != 与 == 同级, 低于 relational ((2>1)=1 → 1!=1 = 0)
        { "1 != 2 > 1", 0u },
        // == 低于 relational ((2>1)=1 → 1==1 = 1; 若漏 relational 层会得 0)
        { "1 == 2 > 1", 1u },
        // relational 低于 shift ((1<<2)=4 → 4>1 = 1; 若反了 1<<(2>1) = 2)
        { "1 << 2 > 1", 1u },
        // shift 低于 + ((16>>(1+1))=4)
        { "16 >> 1 + 1", 4u },
        // + 低于 * ((1+1)<<2 = 8; 若反了 1+(1<<2) = 5)
        { "1 + 1 << 2", 8u },
        // 算术: * 先于 +
        { "2 + 3 * 4", 14u },
        // 一元先于乘法 (-2 * -3 = 6)
        { "-2 * -3", 6u },
    };
    RunMatrix(resp, cases, sizeof(cases) / sizeof(cases[0]));
}

// ── 同级左结合: 连续同优先级运算符从左边开始计算 ──
TEST(MiniExpressionTest, AssociativityIsLeftToRight) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    const PrecedenceCase cases[] = {
        { "10 - 3 - 2", 5u },          // 右结合会得 9
        { "20 / 5 / 2", 2u },          // 右结合会得 10
        { "100 / 10 * 2", 20u },       // 右结合会得 5
        { "8 % 5 % 2", 1u },           // 右结合会得 8%(5%2=1)=0
        { "1 << 1 << 2", 8u },         // 右结合会得 1<<(1<<2)=16
        { "16 >> 2 >> 1", 2u },        // 右结合会得 16>>(2>>1)=8
        { "3 > 2 > 1", 0u },           // (3>2)=1 → 1>1=0; 右结合会得 3>(2>1)=1
        { "2 == 2 == 1", 1u },         // (2==2)=1 → 1==1=1; 右结合会得 2==(2==1)=0
    };
    RunMatrix(resp, cases, sizeof(cases) / sizeof(cases[0]));
}

// ── 一元运算: 绑定最紧, 且可叠加/穿透括号 ──
TEST(MiniExpressionTest, UnaryOperatorsBindTightest) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    const PrecedenceCase cases[] = {
        { "-3 + 5", 2u },              // (-3)+5, 不是 -(3+5)
        { "-2 + 3", 1u },              // 无符号回绕后仍是 1
        { "1 - -3", 4u },              // 减号与一元负号共存
        { "+5", 5u },                  // 一元 + 透传
        { "!0", 1u },
        { "!5", 0u },
        { "!!7", 1u },                 // 双重逻辑非回到 0/1 语义
        { "!1 + 1", 1u },              // (!1)+1 = 1, 不是 !(1+1) = 0
        { "~0 & 0xFF", 255u },         // (~0)&0xFF
        { "~1 & 0xFF", 254u },
        { "~(1 + 2) & 0xFF", 252u },   // 括号让 ~ 作用于整个和
    };
    RunMatrix(resp, cases, sizeof(cases) / sizeof(cases[0]));
}

// ── 括号可覆盖任意层序 (含 resp 索引表达式) ──
TEST(MiniExpressionTest, ParenthesesOverridePrecedence) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    const PrecedenceCase cases[] = {
        { "(1 | 2) * 3", 9u },          // 对比未加括号的 1 | 2 * 3 = 7
        { "(1 || 0) && 0", 0u },        // 对比未加括号的 1 || 0 && 0 = 1
        { "1 + (1 << 2)", 5u },         // 对比未加括号的 1 + 1 << 2 = 8
        { "(2 + 3) * 4", 20u },
        { "(16 >> 1) + 1", 9u },
        { "((((7))))", 7u },
        { "-(1 + 2) + 4", 1u },         // -3+4
        { "resp[1 + 1]", 0x10u },       // 索引内是完整算术表达式
        { "resp[(1 + 1)]", 0x10u },
        { "resp[1] << (1 + 1) | resp[3]", 12u },   // (3<<2)|0
    };
    RunMatrix(resp, cases, sizeof(cases) / sizeof(cases[0]));
}

// ── 真实 validCondition 形态: 组合所有层级仍可读且结果确定 ──
TEST(MiniExpressionTest, CompositeConditionCombinesAllLevels) {
    const Bytes frame = SampleFrame();
    const ByteView resp(frame);

    const PrecedenceCase cases[] = {
        // resp[1]==3 && (((resp[2]>>4)&0xF)==1)
        { "resp[1] == 3 && ((resp[2] >> 4) & 0xF) == 1", 1u },
        // 同上但取低半字节 (0) → 比较失败
        { "resp[1] == 3 && ((resp[2] >> 0) & 0xF) == 1", 0u },
        // 或分支: 前半失败, 后半命中
        { "resp[1] == 9 || resp[2] != 0", 1u },
        // 位拼字 + 比较: ((3<<8)|0x10) == 0x310
        { "(resp[1] << 8 | resp[2]) == 0x310", 1u },
    };
    RunMatrix(resp, cases, sizeof(cases) / sizeof(cases[0]));
}

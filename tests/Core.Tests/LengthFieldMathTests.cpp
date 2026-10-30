// tests/Core.Tests/LengthFieldMathTests.cpp — 长度槽位期望值 (渲染侧单点实现)
//
// 保存期试算 (TrialRenderer) 与 /api/preview 共用 ExpectedLengthFieldValue,
// 两条路径曾各自抄一份公式 —— 这里按 framing 语义锁死边界, 防止再次分叉。

#include "MyProt/Core/Config.hpp"
#include "MiniTest.hpp"

using namespace MyProt::Core;

TEST(LengthFieldMathTest, BodyOnlyExcludesHeader) {
    LengthFieldConfig lf;                 // 默认 includesHeader=false, adj=0
    lf.headerLength = 6;
    EXPECT_EQ(ExpectedLengthFieldValue(lf, 12), 6LL);
}

TEST(LengthFieldMathTest, IncludesHeaderCountsWholeFrame) {
    LengthFieldConfig lf;
    lf.lengthIncludesHeader = true;
    lf.headerLength = 6;                  // includesHeader=true 时 headerLength 不参与
    EXPECT_EQ(ExpectedLengthFieldValue(lf, 12), 12LL);
}

TEST(LengthFieldMathTest, AdjustmentAppliedInBothModes) {
    LengthFieldConfig lf;
    lf.headerLength = 6;
    lf.lengthAdjustment = -2;             // 典型: 帧尾 CRC 不计入长度
    EXPECT_EQ(ExpectedLengthFieldValue(lf, 12), 8LL);    // 12 - 6 - (-2)
    lf.lengthIncludesHeader = true;
    EXPECT_EQ(ExpectedLengthFieldValue(lf, 12), 14LL);   // 12 - (-2)
}

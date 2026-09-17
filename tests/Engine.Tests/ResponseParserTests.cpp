// tests/Engine.Tests/ResponseParserTests.cpp
// ResponseParser 单元测试 — 响应条件校验 ("resp[N]==V" 子集) 与字节序裁决链
// (tag.byteOrder → protocol.dataByteOrder → 大端)。这两个是读/写路径共用的判定点,
// 判错会静默产出错误的标签值 (写路径与读路径走同一裁决链)。

#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Engine/ResponseParser.hpp"
#include "MiniTest.hpp"

using namespace MyProt::Core;
using namespace MyProt::Engine;

namespace {

/// 样本响应: resp[0]=0x01 resp[1]=0x03 resp[2]=0x02 resp[3]=0x00 resp[4]=0x0A
Bytes SampleResponse() {
    Bytes b;
    b.push_back(0x01);
    b.push_back(0x03);
    b.push_back(0x02);
    b.push_back(0x00);
    b.push_back(0x0A);
    return b;
}

} // namespace

// ── 条件校验: 命中 → true; 不命中 → false (无空格与有空格两种写法都要认) ──
TEST(ResponseParserTest, CheckConditionSubset) {
    const Bytes respBytes = SampleResponse();
    const ByteView resp(respBytes);

    EXPECT_TRUE(ResponseParser::CheckCondition(resp, "resp[1]==0x03"));
    EXPECT_FALSE(ResponseParser::CheckCondition(resp, "resp[1]==0x04"));
    EXPECT_TRUE(ResponseParser::CheckCondition(resp, "resp[1] == 0x03"));
    EXPECT_TRUE(ResponseParser::CheckCondition(resp, "resp[4]==10"));   // 十进制写法
}

// ── 字节序裁决: tag 级覆盖优先于协议级 ──
TEST(ResponseParserTest, ResolveByteOrderTagOverridesProtocol) {
    ProtocolConfig proto;
    proto.dataByteOrder = ByteOrder::BigEndian;

    // 标签未指定 → 取协议级
    EXPECT_TRUE(ResponseParser::ResolveByteOrder(proto, Optional<ByteOrder>())
                == ByteOrder::BigEndian);

    // 标签指定 → 覆盖协议级 (混合字序, 如 Melsec/BADC 设备)
    EXPECT_TRUE(ResponseParser::ResolveByteOrder(proto, Optional<ByteOrder>(ByteOrder::WordLittleByteBig))
                == ByteOrder::WordLittleByteBig);
    EXPECT_TRUE(ResponseParser::ResolveByteOrder(proto, Optional<ByteOrder>(ByteOrder::LittleEndian))
                == ByteOrder::LittleEndian);
}

// ── 字节序裁决: 协议级随配置变化 (证明优先级链没有把协议级写死成大端) ──
TEST(ResponseParserTest, ResolveByteOrderProtocolLevelNotHardcoded) {
    ProtocolConfig proto;
    proto.dataByteOrder = ByteOrder::WordBigByteLittle;   // CDAB

    EXPECT_TRUE(ResponseParser::ResolveByteOrder(proto, Optional<ByteOrder>())
                == ByteOrder::WordBigByteLittle);
}

// ── v5 (C1) 读后换算链 ──
// 本地帧: 6 字节, dataStartIndex=4 起 2 字节 = 0x0A 0x00 → 小端 UInt16 = 10
static Bytes ConvFrame() {
    Bytes b;
    b.push_back(0x01);
    b.push_back(0x03);
    b.push_back(0x02);
    b.push_back(0x00);
    b.push_back(0x0A);
    b.push_back(0x00);
    return b;
}

TEST(ResponseParserTest, ConvertersScaleAppliedInOrder) {
    const Bytes respBytes = ConvFrame();
    const ByteView resp(respBytes);

    ResponseParserConfig cfg;
    cfg.dataStartIndex = 4;

    TagDefinition tag;
    tag.name = "T1";
    tag.finalType = "UInt16";
    tag.variables[ByteCountVariableName()] = 2;
    ScaleConverter c1; c1.k = 0.1; c1.b = 0.0;    // 10 -> 1.0
    ScaleConverter c2; c2.k = 1.0; c2.b = -5.0;   // 1.0 -> -4.0 (链式: 先乘 k 后加 b)
    tag.converters.push_back(c1);
    tag.converters.push_back(c2);

    ResponseParser rp;
    const Expected<TagValue> tv = rp.Parse(resp, cfg, tag, ByteOrder::LittleEndian);
    ASSERT_TRUE(tv.has_value());
    EXPECT_TRUE(tv.value().typedValue.type == ValueType::Double);
    EXPECT_TRUE(tv.value().typedValue.d > -4.0001 && tv.value().typedValue.d < -3.9999);
    EXPECT_TRUE(tv.value().quality == QualityCode::Good);
}

// 空链 = 行为与旧版完全一致 (UInt16 原样)
TEST(ResponseParserTest, ConvertersEmptyChainUnchanged) {
    const Bytes respBytes = ConvFrame();
    const ByteView resp(respBytes);

    ResponseParserConfig cfg;
    cfg.dataStartIndex = 4;

    TagDefinition tag;
    tag.name = "T2";
    tag.finalType = "UInt16";
    tag.variables[ByteCountVariableName()] = 2;

    ResponseParser rp;
    const Expected<TagValue> tv = rp.Parse(resp, cfg, tag, ByteOrder::LittleEndian);
    ASSERT_TRUE(tv.has_value());
    EXPECT_TRUE(tv.value().typedValue.type == ValueType::UInt16);
    EXPECT_TRUE(tv.value().typedValue.u == 10);
}

// 非数值 finalType (Bool) — 声明 converters 被校验器拦截; 引擎侧防御性跳过换算,
// 类型保持 Bool (raw[0]=0x0A 非 0 -> true)
TEST(ResponseParserTest, ConvertersNonNumericSkippedDefensively) {
    const Bytes respBytes = ConvFrame();
    const ByteView resp(respBytes);

    ResponseParserConfig cfg;
    cfg.dataStartIndex = 4;

    TagDefinition tag;
    tag.name = "T3";
    tag.finalType = "Bool";
    ScaleConverter c; c.k = 2.0; c.b = 1.0;
    tag.converters.push_back(c);

    ResponseParser rp;
    const Expected<TagValue> tv = rp.Parse(resp, cfg, tag, ByteOrder::LittleEndian);
    ASSERT_TRUE(tv.has_value());
    EXPECT_TRUE(tv.value().typedValue.type == ValueType::Bool);
    EXPECT_TRUE(tv.value().typedValue.b == true);
}
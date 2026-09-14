// tests/Engine.Tests/ResponseParserTests.cpp
// ResponseParser 单元测试 — 响应条件校验 ("resp[N]==V" 子集) 与字节序裁决链
// (tag.byteOrder → protocol.dataByteOrder → 大端)。这两个是读/写路径共用的判定点,
// 判错会静默产出错误的标签值 (v1.28 起写路径也走同一裁决链)。

#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/ByteView.hpp"
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

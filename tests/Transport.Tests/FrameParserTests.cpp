// tests/Transport.Tests/FrameParserTests.cpp — 帧解析器单元测试
// v1 撤回 FixedFrameParser (A5) — TcpChannel 走内联 f.fixed.fixedLength, 不再独立类

#include "MyProt/Transport/LengthFieldFrameParser.hpp"
#include <gtest/gtest.h>

using namespace MyProt::Transport;
using namespace MyProt::Core;

// ─── LengthFieldFrameParser ───
// Modbus TCP 帧: transId(2)+protoId(2)+length(2)+unitId(1)+FC(1)+byteCount(1)+data(6) = 15
// length 字段值 = 9 (unitId+FC+byteCount+data)
// lengthIncludesHeader=false → totalFrameLen = headerLength(6) + parsedLen(9) + adjustment(0)

static LengthFieldConfig MakeModbusTcpConfig() {
    LengthFieldConfig config;
    config.lengthFieldOffset = 4;
    config.lengthFieldLength = 2;
    config.lengthIncludesHeader = false;
    config.byteOrder = ByteOrder::BigEndian;
    config.headerLength = 6;
    config.lengthAdjustment = 0;
    config.maxFrameSize = 260;
    return config;
}

TEST(LengthFieldFrameParserTest, ParseModbusTcpFrame) {
    LengthFieldFrameParser parser(MakeModbusTcpConfig());

    uint8_t frame[] = {0x00,0x01, 0x00,0x00, 0x00,0x09, 0x01,0x03,0x06, 0x00,0x64,0x00,0x65,0x00,0x66};
    ByteView view(frame, 15);

    EXPECT_TRUE(parser.HasCompleteFrame(view));

    auto result = parser.Parse(view, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().needMoreData);
    EXPECT_EQ(result.value().frame.size(), 15u);
    EXPECT_EQ(result.value().consumedBytes, 15u);
}

TEST(LengthFieldFrameParserTest, ParseTruncatedFrameNeedsMoreData) {
    LengthFieldFrameParser parser(MakeModbusTcpConfig());

    uint8_t frame[] = {0x00,0x01, 0x00,0x00, 0x00,0x09, 0x01,0x03};
    ByteView view(frame, 8);

    EXPECT_FALSE(parser.HasCompleteFrame(view));

    auto result = parser.Parse(view, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result.value().needMoreData);
}

TEST(LengthFieldFrameParserTest, ParseOversizedFrameFails) {
    LengthFieldFrameParser parser(MakeModbusTcpConfig());

    // length = 0xFFFF → totalFrameLen 远超 maxFrameSize(260)
    uint8_t frame[] = {0x00,0x01, 0x00,0x00, 0xFF,0xFF};
    ByteView view(frame, 6);

    auto result = parser.Parse(view, 0);
    EXPECT_FALSE(result.has_value());
}

TEST(LengthFieldFrameParserTest, LittleEndianLength) {
    LengthFieldConfig config = MakeModbusTcpConfig();
    config.byteOrder = ByteOrder::LittleEndian;
    LengthFieldFrameParser parser(config);

    // length 字段小端: 09 00 → 9
    uint8_t frame[] = {0x00,0x01, 0x00,0x00, 0x09,0x00, 0x01,0x03,0x06, 0x00,0x64,0x00,0x65,0x00,0x66};
    ByteView view(frame, 15);

    auto result = parser.Parse(view, 0);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result.value().needMoreData);
    EXPECT_EQ(result.value().frame.size(), 15u);
}

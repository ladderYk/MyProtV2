// tests/Transport.Tests/StreamFrameSpecTests.cpp
// Config-driven framing primitives (ADR-0021): varint length, bit fields,
// dispatch and repeated records. Everything is exercised through Core::IFrameCodec,
// so the test does not depend on TcpIngestServer.

#include "MyProt/Transport/StreamFrameCodec.hpp"
#include "MiniTest.hpp"

#include <string>
#include <vector>

using namespace MyProt::Core;
using namespace MyProt::Transport;

namespace {

FramingConfig MakeVarintSpec() {
    FramingConfig cfg;
    cfg.type = FramingType::LengthField;
    LengthFieldConfig& lf = cfg.lengthField;
    lf.lengthFieldOffset = 1;
    lf.lengthFieldLength = 0;              // Varint: width comes from the encoding
    lf.lengthEncoding = LengthFieldEncoding::Varint;
    lf.varintMaxBytes = 4;
    lf.lengthIncludesHeader = false;
    lf.headerLength = 0;                   // dynamic header = offset + varint width
    lf.maxFrameSize = 1024;

    HeaderFieldConfig type;
    type.name = "type";
    type.bitOffset = 0;
    type.bitWidth = 4;
    HeaderFieldConfig flags;
    flags.name = "flags";
    flags.bitOffset = 4;
    flags.bitWidth = 4;
    lf.headerFields.push_back(type);
    lf.headerFields.push_back(flags);

    DispatchCaseConfig publish;
    publish.value = 3;
    publish.type = 3;
    publish.name = "Publish";
    lf.dispatch.field = "type";
    lf.dispatch.rejectUnknown = true;
    lf.dispatch.cases.push_back(publish);
    return cfg;
}

std::vector<uint8_t> PublishFrame(const std::string& payload, uint8_t firstByte) {
    std::vector<uint8_t> bytes;
    bytes.push_back(firstByte);                       // type/flags nibbles
    const size_t remaining = payload.size();
    if (remaining < 128) {
        bytes.push_back(static_cast<uint8_t>(remaining));
    } else {
        bytes.push_back(static_cast<uint8_t>((remaining & 0x7F) | 0x80));
        bytes.push_back(static_cast<uint8_t>(remaining >> 7));
    }
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool Contains(const std::vector<uint8_t>& bytes, const std::string& text) {
    return std::string(bytes.begin(), bytes.end()).find(text) != std::string::npos;
}

} // namespace

// ─── varint length ───

TEST(StreamFrameSpecTest, VarintLengthSplitsFrameAndFillsBits) {
    StreamFrameCodec codec(MakeVarintSpec());
    const std::vector<uint8_t> wire = PublishFrame("hello", 0x32);  // type=3, flags=2

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Complete);
    EXPECT_EQ(consumed, 7u);                 // 1 header + 1 varint + 5 payload

    uint64_t type = 0;
    uint64_t flags = 0;
    EXPECT_TRUE(frame.GetUInt("type", type));
    EXPECT_TRUE(frame.GetUInt("flags", flags));
    EXPECT_EQ(type, 3u);
    EXPECT_EQ(flags, 2u);
    EXPECT_EQ(frame.type, 3u);
    EXPECT_EQ(frame.typeName, std::string("Publish"));
    EXPECT_EQ(frame.payload.size, 5u);
    EXPECT_TRUE(Contains(std::vector<uint8_t>(frame.payload.data,
                                              frame.payload.data + frame.payload.size),
                         "hello"));
}

TEST(StreamFrameSpecTest, VarintLengthSupportsTwoBytes) {
    StreamFrameCodec codec(MakeVarintSpec());
    const std::string payload(200, 'x');
    const std::vector<uint8_t> wire = PublishFrame(payload, 0x30);

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Complete);
    EXPECT_EQ(consumed, 203u);               // 1 + 2 + 200
    EXPECT_EQ(frame.payload.size, 200u);
}

TEST(StreamFrameSpecTest, TruncatedVarintNeedsMoreData) {
    StreamFrameCodec codec(MakeVarintSpec());
    const uint8_t wire[] = {0x30, 0x80};     // continuation bit set, no next byte

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(wire, sizeof(wire)), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::NeedMore);
    EXPECT_EQ(consumed, 0u);
}

TEST(StreamFrameSpecTest, VarintLongerThanMaxBytesIsMalformed) {
    StreamFrameCodec codec(MakeVarintSpec());
    const uint8_t wire[] = {0x30, 0x80, 0x80, 0x80, 0x80};

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(wire, sizeof(wire)), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Malformed);
}

// ─── dispatch ───

TEST(StreamFrameSpecTest, UnknownDispatchValueIsMalformed) {
    StreamFrameCodec codec(MakeVarintSpec());
    const std::vector<uint8_t> wire = PublishFrame("x", 0x70);  // type=7, no case

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Malformed);
}

TEST(StreamFrameSpecTest, UnknownDispatchValueCanFallBackToRawType) {
    FramingConfig cfg = MakeVarintSpec();
    cfg.lengthField.dispatch.rejectUnknown = false;
    StreamFrameCodec codec(cfg);
    const std::vector<uint8_t> wire = PublishFrame("x", 0x70);

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Complete);
    EXPECT_EQ(frame.type, 7u);
    EXPECT_TRUE(frame.typeName.empty());
}

// ─── repeat ───

TEST(StreamFrameSpecTest, RepeatRegionIsExposedAsBytes) {
    FramingConfig cfg = MakeVarintSpec();
    HeaderFieldConfig count;
    count.name = "count";
    count.bitOffset = 16;                    // byte 2 (bytes 0..1 are header + varint length)
    count.bitWidth = 8;
    cfg.lengthField.headerFields.push_back(count);

    RepeatConfig& repeat = cfg.lengthField.repeat;
    repeat.countField = "count";
    repeat.stride = 2;
    repeat.offset = 3;                       // first record right after the count byte
    repeat.name = "records";
    repeat.maxCount = 16;

    StreamFrameCodec codec(cfg);

    // type=3, length=7, count=3, then three 2-byte records "ab" "cd" "ef"
    std::vector<uint8_t> wire;
    wire.push_back(0x30);
    wire.push_back(0x07);                    // remaining = count byte + 6 record bytes
    wire.push_back(0x03);
    wire.push_back('a'); wire.push_back('b');
    wire.push_back('c'); wire.push_back('d');
    wire.push_back('e'); wire.push_back('f');

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Complete);
    ByteView records;
    EXPECT_TRUE(frame.GetBytes("records", records));
    EXPECT_EQ(records.size, 6u);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(records.data), records.size),
              std::string("abcdef"));
}

TEST(StreamFrameSpecTest, RepeatCountOverflowsFrameIsMalformed) {
    FramingConfig cfg = MakeVarintSpec();
    HeaderFieldConfig count;
    count.name = "count";
    count.bitOffset = 16;                    // byte 2
    count.bitWidth = 8;
    cfg.lengthField.headerFields.push_back(count);

    RepeatConfig& repeat = cfg.lengthField.repeat;
    repeat.countField = "count";
    repeat.stride = 2;
    repeat.offset = 3;
    repeat.maxCount = 16;

    StreamFrameCodec codec(cfg);

    std::vector<uint8_t> wire;
    wire.push_back(0x30);
    wire.push_back(0x03);                    // body = count + 2 bytes
    wire.push_back(0x05);                    // claims 5 records = 10 bytes
    wire.push_back('a'); wire.push_back('b');

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(&wire[0], wire.size()), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Malformed);
}

// ─── Fixed framing remains untouched ───

TEST(StreamFrameSpecTest, FixedFramingStillReturnsWholeFrame) {
    FramingConfig cfg;
    cfg.type = FramingType::Fixed;
    cfg.fixed.fixedLength = 4;
    StreamFrameCodec codec(cfg);
    const uint8_t wire[] = {1, 2, 3, 4, 5};

    Frame frame;
    size_t consumed = 0;
    const DecodeStatus st = codec.Decode(ByteView(wire, sizeof(wire)), consumed, frame);

    EXPECT_EQ(st, DecodeStatus::Complete);
    EXPECT_EQ(consumed, 4u);
    EXPECT_EQ(frame.payload.size, 4u);
    EXPECT_EQ(frame.type, 0u);
}

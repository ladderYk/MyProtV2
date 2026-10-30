// tests/Northbound.Tests/MqttSessionTests.cpp — 帧层 / 会话层切分测试 (ADR-0020)
//
// 覆盖两件事：
//   1) MqttCodec 作为 Core::IFrameCodec 的通用解码/编码（不经过会话）；
//   2) MqttSession 只消费 Core::Frame，协议动作可脱离 socket 单测。

#include "MyProt/Northbound/MqttCodec.hpp"
#include "MyProt/Northbound/MqttSession.hpp"
#include "MiniTest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using MyProt::Core::ByteView;
using MyProt::Core::DecodeStatus;
using MyProt::Core::Frame;
using MyProt::Northbound::MqttCodec;
using MyProt::Northbound::MqttSession;

ByteView View(const std::vector<uint8_t>& bytes) {
    return ByteView(bytes.empty() ? 0 : &bytes[0], bytes.size());
}

bool DecodeOne(MqttCodec& codec, const std::vector<uint8_t>& bytes, Frame& frame) {
    std::size_t consumed = 0;
    const DecodeStatus status = codec.Decode(View(bytes), consumed, frame);
    return status == DecodeStatus::Complete && consumed == bytes.size();
}

} // namespace

// ──────────────────── 帧层 ────────────────────

TEST(MqttFrameTest, PublishFieldsAndPayloadAreDecoded) {
    MqttCodec codec;
    const std::vector<uint8_t> bytes =
        MqttCodec::EncodePublish("dev/tag", "hi", 1, true, 0x0102);

    Frame frame;
    ASSERT_TRUE(DecodeOne(codec, bytes, frame));
    EXPECT_EQ(frame.type, 3u);          // PUBLISH
    EXPECT_EQ(frame.flags, 0x03u);      // qos1 + retain

    std::string topic;
    uint64_t qos = 9;
    uint64_t packetId = 0;
    bool retain = false;
    EXPECT_TRUE(frame.GetString("topic", topic));
    EXPECT_EQ(topic, std::string("dev/tag"));
    EXPECT_TRUE(frame.GetUInt("qos", qos));
    EXPECT_EQ(qos, 1u);
    EXPECT_TRUE(frame.GetUInt("packetId", packetId));
    EXPECT_EQ(packetId, 0x0102u);
    EXPECT_TRUE(frame.GetBool("retain", retain));
    EXPECT_TRUE(retain);

    ASSERT_TRUE(frame.payload.size == 2);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(frame.payload.data),
                          frame.payload.size),
              std::string("hi"));
}

TEST(MqttFrameTest, ConnackAndSubackDecodeToFields) {
    MqttCodec codec;

    const uint8_t connackBytes[4] = { 0x20, 0x02, 0x01, 0x00 };
    const std::vector<uint8_t> connack(connackBytes, connackBytes + 4);
    Frame frame;
    ASSERT_TRUE(DecodeOne(codec, connack, frame));
    EXPECT_EQ(frame.type, 2u);
    bool sessionPresent = false;
    uint64_t returnCode = 0xFF;
    EXPECT_TRUE(frame.GetBool("sessionPresent", sessionPresent));
    EXPECT_TRUE(sessionPresent);
    EXPECT_TRUE(frame.GetUInt("returnCode", returnCode));
    EXPECT_EQ(returnCode, 0u);

    const uint8_t subackBytes[5] = { 0x90, 0x03, 0x00, 0x07, 0x01 };
    const std::vector<uint8_t> suback(subackBytes, subackBytes + 5);
    ASSERT_TRUE(DecodeOne(codec, suback, frame));
    uint64_t packetId = 0;
    ByteView codes;
    EXPECT_TRUE(frame.GetUInt("packetId", packetId));
    EXPECT_EQ(packetId, 7u);
    EXPECT_TRUE(frame.GetBytes("returnCodes", codes));
    ASSERT_TRUE(codes.size == 1);
    EXPECT_EQ(static_cast<int>(codes[0]), 1);
}

TEST(MqttFrameTest, IncompleteBufferReportsNeedMore) {
    MqttCodec codec;
    const std::vector<uint8_t> bytes =
        MqttCodec::EncodePublish("a/b", "payload", 0, false, 0);

    std::vector<uint8_t> partial(bytes.begin(), bytes.begin() + 4);
    Frame frame;
    std::size_t consumed = 99;
    EXPECT_TRUE(codec.Decode(View(partial), consumed, frame) == DecodeStatus::NeedMore);
    EXPECT_EQ(consumed, static_cast<std::size_t>(0));

    // 非法类型 (保留 0x00) 必须与"未收全"区分开
    std::vector<uint8_t> reserved(2, 0x00);
    EXPECT_TRUE(codec.Decode(View(reserved), consumed, frame) == DecodeStatus::Malformed);
}

TEST(MqttFrameTest, EncodeFromFrameMatchesTypedEncoder) {
    MqttCodec codec;

    Frame publish;
    publish.type = 3;
    publish.SetString("topic", "a/b");
    publish.SetUInt("qos", 0);
    publish.SetBool("retain", false);
    const std::string payload = "hi";
    publish.payload = ByteView(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());

    std::vector<uint8_t> encoded;
    ASSERT_TRUE(codec.Encode(publish, encoded));
    EXPECT_TRUE(encoded == MqttCodec::EncodePublish("a/b", "hi", 0, false, 0));

    // 不支持的报文类型必须显式失败，而不是产出空帧
    Frame unknown;
    unknown.type = 42;
    EXPECT_FALSE(codec.Encode(unknown, encoded));
}

// ──────────────────── 会话层 ────────────────────

TEST(MqttSessionTest, ConnackAcceptedThenRejected) {
    MqttCodec codec;
    MyProt::Core::MqttConfig cfg;
    cfg.clientId = "gw-1";
    MqttSession session(codec, cfg);

    const uint8_t okBytes[4] = { 0x20, 0x02, 0x00, 0x00 };
    const std::vector<uint8_t> ok(okBytes, okBytes + 4);
    Frame frame;
    ASSERT_TRUE(DecodeOne(codec, ok, frame));
    EXPECT_TRUE(session.OnFrame(frame) == MqttSession::Event::Connected);
    EXPECT_TRUE(session.LastError().empty());

    const uint8_t badBytes[4] = { 0x20, 0x02, 0x00, 0x05 };
    const std::vector<uint8_t> bad(badBytes, badBytes + 4);
    ASSERT_TRUE(DecodeOne(codec, bad, frame));
    EXPECT_TRUE(session.OnFrame(frame) == MqttSession::Event::Fatal);
    EXPECT_TRUE(session.LastError().find("未授权") != std::string::npos);
}

TEST(MqttSessionTest, PublishQos1EmitsPubackAndMessage) {
    MqttCodec codec;
    MyProt::Core::MqttConfig cfg;
    MqttSession session(codec, cfg);

    std::string gotTopic;
    std::string gotPayload;
    session.SetOnMessage([&gotTopic, &gotPayload](
            const std::string& topic, const std::string& payload) {
        gotTopic = topic;
        gotPayload = payload;
    });

    std::vector<std::vector<uint8_t> > sent;
    session.SetSendHandler([&sent](const std::vector<uint8_t>& packet) {
        sent.push_back(packet);
    });

    const std::vector<uint8_t> bytes =
        MqttCodec::EncodePublish("dev/tag", "42", 1, false, 0x0102);
    Frame frame;
    ASSERT_TRUE(DecodeOne(codec, bytes, frame));

    EXPECT_TRUE(session.OnFrame(frame) == MqttSession::Event::None);
    EXPECT_EQ(gotTopic, std::string("dev/tag"));
    EXPECT_EQ(gotPayload, std::string("42"));
    ASSERT_TRUE(sent.size() == 1);
    EXPECT_TRUE(sent[0] == MqttCodec::EncodePuback(0x0102));
}

TEST(MqttSessionTest, SubscriptionsAreReplayedAfterReconnect) {
    MqttCodec codec;
    MyProt::Core::MqttConfig cfg;
    MqttSession session(codec, cfg);

    session.AddSubscription("plant/+/data", 1);
    session.AddSubscription("plant/+/alarm", 0);
    session.AddSubscription("plant/+/data", 1);   // 重复声明 → 覆盖, 不重复订阅

    const std::vector<std::vector<uint8_t> > packets =
        session.BuildSubscriptionPackets();
    ASSERT_TRUE(packets.size() == 2);

    // 每个 SUBSCRIBE 都是完整报文: 固定头 0x82 + 变长剩余长度
    EXPECT_EQ(static_cast<int>(packets[0][0]), 0x82);
    EXPECT_EQ(static_cast<int>(packets[1][0]), 0x82);

    // 第二次连接必须生成新的 packetId, 不复用旧值
    const std::vector<std::vector<uint8_t> > again =
        session.BuildSubscriptionPackets();
    ASSERT_TRUE(again.size() == 2);
    EXPECT_FALSE(again[0] == packets[0]);
}

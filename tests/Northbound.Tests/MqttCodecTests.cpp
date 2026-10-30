// tests/Northbound.Tests/MqttCodecTests.cpp — MQTT 3.1.1 报文编解码单元测试

#include "MyProt/Northbound/MqttCodec.hpp"
#include "MiniTest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using MyProt::Northbound::MqttCodec;
using MyProt::Northbound::MqttDecodeResult;
using MyProt::Northbound::MqttPacket;
using MyProt::Northbound::MqttPacketType;

std::vector<uint8_t> Varint(uint32_t v) {
    std::vector<uint8_t> out;
    MqttCodec::AppendVarint(out, v);
    return out;
}

} // namespace

TEST(MqttCodecTest, VarintBoundaries) {
    EXPECT_EQ(Varint(0), std::vector<uint8_t>(1, 0x00));
    EXPECT_EQ(Varint(127), std::vector<uint8_t>(1, 0x7F));

    const uint8_t two[2] = { 0x80, 0x01 };
    EXPECT_EQ(Varint(128), std::vector<uint8_t>(two, two + 2));

    const uint8_t three[3] = { 0xFF, 0xFF, 0x03 };   // 65535
    EXPECT_EQ(Varint(65535), std::vector<uint8_t>(three, three + 3));

    const uint8_t maxv[4] = { 0xFF, 0xFF, 0xFF, 0x7F };   // 268435455
    EXPECT_EQ(Varint(268435455u), std::vector<uint8_t>(maxv, maxv + 4));
}

TEST(MqttCodecTest, ConnectPacketLayout) {
    const std::vector<uint8_t> p = MqttCodec::EncodeConnect(
        "gw-1", "user", "pass", 60, true, "", "", 0, false);

    ASSERT_TRUE(p.size() > 12);
    EXPECT_EQ(static_cast<int>(p[0]), 0x10);          // CONNECT, flags 0
    EXPECT_EQ(static_cast<int>(p[2]), 0x00);          // 协议名长度高字节
    EXPECT_EQ(static_cast<int>(p[3]), 0x04);
    EXPECT_EQ(static_cast<char>(p[4]), 'M');
    EXPECT_EQ(static_cast<char>(p[7]), 'T');
    EXPECT_EQ(static_cast<int>(p[8]), 0x04);          // 协议级别 3.1.1

    // connect flags: username(0x80) | password(0x40) | cleanSession(0x02)
    EXPECT_EQ(static_cast<int>(p[9]), 0x80 | 0x40 | 0x02);

    // keepalive: 60 秒大端
    EXPECT_EQ(static_cast<int>(p[10]), 0x00);
    EXPECT_EQ(static_cast<int>(p[11]), 60);

    // clientId 长度 4 + "gw-1"
    EXPECT_EQ(static_cast<int>(p[12]), 0x00);
    EXPECT_EQ(static_cast<int>(p[13]), 0x04);
    EXPECT_EQ(static_cast<char>(p[14]), 'g');
    EXPECT_EQ(static_cast<char>(p[17]), '1');
}

TEST(MqttCodecTest, ConnectWithWill) {
    const std::vector<uint8_t> p = MqttCodec::EncodeConnect(
        "gw", "", "", 30, true, "myprot/status", "offline", 1, true);

    ASSERT_TRUE(p.size() > 10);
    const uint8_t flags = p[9];
    EXPECT_TRUE((flags & 0x04) != 0);   // will flag
    EXPECT_TRUE((flags & 0x20) != 0);   // will retain
    EXPECT_EQ(static_cast<int>((flags >> 3) & 0x03), 1);   // will qos
    EXPECT_FALSE((flags & 0x80) != 0);  // 无 username
    EXPECT_FALSE((flags & 0x40) != 0);  // 无 password
}

TEST(MqttCodecTest, PublishQos0AndQos1) {
    const std::vector<uint8_t> q0 =
        MqttCodec::EncodePublish("a/b", "hi", 0, false, 0);
    ASSERT_TRUE(q0.size() == 9);
    EXPECT_EQ(static_cast<int>(q0[0]), 0x30);          // PUBLISH, qos0, no retain
    EXPECT_EQ(static_cast<int>(q0[1]), 7);             // remaining length
    EXPECT_EQ(static_cast<int>(q0[2]), 0x00);          // topic len hi
    EXPECT_EQ(static_cast<int>(q0[3]), 0x03);
    EXPECT_EQ(static_cast<char>(q0[4]), 'a');
    EXPECT_EQ(static_cast<char>(q0[6]), 'b');
    EXPECT_EQ(static_cast<char>(q0[7]), 'h');
    EXPECT_EQ(static_cast<char>(q0[8]), 'i');

    const std::vector<uint8_t> q1 =
        MqttCodec::EncodePublish("a/b", "hi", 1, true, 0x1234);
    EXPECT_EQ(static_cast<int>(q1[0]), 0x33);          // qos1 + retain
    EXPECT_EQ(static_cast<int>(q1[7]), 0x12);          // packetId 大端
    EXPECT_EQ(static_cast<int>(q1[8]), 0x34);
}

TEST(MqttCodecTest, ControlPackets) {
    const std::vector<uint8_t> puback = MqttCodec::EncodePuback(0x0102);
    ASSERT_TRUE(puback.size() == 4);
    EXPECT_EQ(static_cast<int>(puback[0]), 0x40);
    EXPECT_EQ(static_cast<int>(puback[1]), 0x02);
    EXPECT_EQ(static_cast<int>(puback[2]), 0x01);
    EXPECT_EQ(static_cast<int>(puback[3]), 0x02);

    const std::vector<uint8_t> ping = MqttCodec::EncodePingreq();
    ASSERT_TRUE(ping.size() == 2);
    EXPECT_EQ(static_cast<int>(ping[0]), 0xC0);
    EXPECT_EQ(static_cast<int>(ping[1]), 0x00);

    const std::vector<uint8_t> disc = MqttCodec::EncodeDisconnect();
    ASSERT_TRUE(disc.size() == 2);
    EXPECT_EQ(static_cast<int>(disc[0]), 0xE0);
}

TEST(MqttCodecTest, SubscribePacketAndSuback) {
    const std::vector<uint8_t> sub = MqttCodec::EncodeSubscribe("plant/dev1/data", 1, 0x0102);
    // 固定头 0x82 + 剩余长度; 报文体 = packetId(2) + topicLen(2) + topic + qos(1)
    ASSERT_TRUE(sub.size() == 2 + 2 + 2 + 15 + 1);
    EXPECT_EQ(static_cast<int>(sub[0]), 0x82);          // SUBSCRIBE | flags(0x02)
    EXPECT_EQ(static_cast<int>(sub[1]), 20);            // remaining length
    EXPECT_EQ(static_cast<int>(sub[2]), 0x01);          // packetId hi
    EXPECT_EQ(static_cast<int>(sub[3]), 0x02);          // packetId lo
    EXPECT_EQ(static_cast<int>(sub[4]), 0x00);          // topicLen hi
    EXPECT_EQ(static_cast<int>(sub[5]), 15);            // topicLen lo
    EXPECT_EQ(static_cast<int>(sub[sub.size() - 1]), 1);  // requested QoS

    MqttPacket p;
    p.type = MqttPacketType::Suback;
    p.flags = 0;
    p.body.assign(3, 0);
    p.body[0] = 0x01;
    p.body[1] = 0x02;
    p.body[2] = 0x00;
    uint16_t packetId = 0;
    std::vector<uint8_t> codes;
    EXPECT_TRUE(MqttCodec::ParseSuback(p, packetId, codes));
    EXPECT_EQ(static_cast<int>(packetId), 0x0102);
    ASSERT_TRUE(codes.size() == 1);
    EXPECT_EQ(static_cast<int>(codes[0]), 0);
}

TEST(MqttCodecTest, DecodeIncremental) {
    const std::vector<uint8_t> frame =
        MqttCodec::EncodePublish("dev/tag", "payload", 0, false, 0);

    // 只给固定头 → 还缺报文体
    std::vector<uint8_t> partial(frame.begin(), frame.begin() + 2);
    MqttPacket p;
    std::size_t consumed = 0;
    EXPECT_TRUE(MqttCodec::TryDecode(partial, 0, p, consumed)
                == MqttDecodeResult::NeedMore);

    // 给全量 → Complete, consumed 覆盖整个报文
    EXPECT_TRUE(MqttCodec::TryDecode(frame, 0, p, consumed)
                == MqttDecodeResult::Complete);
    EXPECT_EQ(consumed, frame.size());
    EXPECT_TRUE(p.type == MqttPacketType::Publish);

    std::string topic;
    uint16_t packetId = 0;
    std::size_t payloadOffset = 0;
    ASSERT_TRUE(MqttCodec::ParsePublish(p, topic, packetId, payloadOffset));
    EXPECT_EQ(topic, std::string("dev/tag"));
    EXPECT_EQ(static_cast<int>(packetId), 0);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(&p.body[payloadOffset]),
                          p.body.size() - payloadOffset),
              std::string("payload"));
}

TEST(MqttCodecTest, DecodeTwoPacketsInOneBuffer) {
    // PINGRESP (客户端会收到; PINGREQ 只由客户端发出)
    const uint8_t pingBytes[2] = { 0xD0, 0x00 };
    const std::vector<uint8_t> pingresp(pingBytes, pingBytes + 2);
    const std::vector<uint8_t> b = MqttCodec::EncodePuback(7);
    std::vector<uint8_t> buf(pingresp);
    buf.insert(buf.end(), b.begin(), b.end());

    MqttPacket p;
    std::size_t consumed = 0;
    ASSERT_TRUE(MqttCodec::TryDecode(buf, 0, p, consumed)
                == MqttDecodeResult::Complete);
    EXPECT_TRUE(p.type == MqttPacketType::Pingresp);
    EXPECT_EQ(consumed, pingresp.size());

    ASSERT_TRUE(MqttCodec::TryDecode(buf, consumed, p, consumed)
                == MqttDecodeResult::Complete);
    EXPECT_TRUE(p.type == MqttPacketType::Puback);
    EXPECT_EQ(static_cast<int>(p.body[0]), 0x00);
    EXPECT_EQ(static_cast<int>(p.body[1]), 0x07);
}

TEST(MqttCodecTest, DecodeRejectsMalformed) {
    MqttPacket p;
    std::size_t consumed = 0;

    // 保留类型 (0x00) → Malformed
    std::vector<uint8_t> reserved(2, 0x00);
    EXPECT_TRUE(MqttCodec::TryDecode(reserved, 0, p, consumed)
                == MqttDecodeResult::Malformed);

    // 剩余长度超过 4 字节 → Malformed
    std::vector<uint8_t> longLen;
    longLen.push_back(0x30);
    longLen.push_back(0x80);
    longLen.push_back(0x80);
    longLen.push_back(0x80);
    longLen.push_back(0x80);
    longLen.push_back(0x01);
    EXPECT_TRUE(MqttCodec::TryDecode(longLen, 0, p, consumed)
                == MqttDecodeResult::Malformed);
}

TEST(MqttCodecTest, ParseConnack) {
    MqttPacket p;
    p.type = MqttPacketType::Connack;
    p.body.push_back(0x01);   // sessionPresent
    p.body.push_back(0x00);   // accepted

    uint8_t code = 0xFF;
    bool sessionPresent = false;
    ASSERT_TRUE(MqttCodec::ParseConnack(p, code, sessionPresent));
    EXPECT_EQ(static_cast<int>(code), 0);
    EXPECT_TRUE(sessionPresent);

    p.body.resize(1);         // 截断报文
    EXPECT_FALSE(MqttCodec::ParseConnack(p, code, sessionPresent));
}

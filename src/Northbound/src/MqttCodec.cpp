// src/Northbound/src/MqttCodec.cpp
// MQTT 3.1.1 报文编解码实现 — 纯字节操作, 无 IO / 无线程 (便于单测)

#include "MyProt/Northbound/MqttCodec.hpp"

#include <cstddef>

namespace MyProt { namespace Northbound {

namespace {

const std::size_t kMaxRemainingLengthBytes = 4;
const uint32_t kMaxRemainingLength = 268435455u;   // 0xFFFFFF7F 编码上限

inline void AppendU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

inline uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

/// 切出固定头 + 变长剩余长度 (MQTT 3.1.1 §2.2)。只接受本模块会收到的类型。
/// 不变式: Complete 时 bodyOffset + remaining == total。
Core::DecodeStatus ParseFixedHeader(const uint8_t* data, std::size_t size,
                                    std::size_t offset, uint8_t& typeRaw,
                                    uint8_t& flags, std::size_t& bodyOffset,
                                    std::size_t& total) {
    if (offset >= size) return Core::DecodeStatus::NeedMore;
    if (size - offset < 2) return Core::DecodeStatus::NeedMore;

    const uint8_t header = data[offset];
    typeRaw = static_cast<uint8_t>((header >> 4) & 0x0F);
    flags = static_cast<uint8_t>(header & 0x0F);

    switch (typeRaw) {
    case 2: case 3: case 4: case 9: case 13:
        break;
    default:
        return Core::DecodeStatus::Malformed;
    }

    uint32_t remaining = 0;
    uint32_t multiplier = 1;
    std::size_t pos = offset + 1;
    std::size_t lenBytes = 0;
    for (;;) {
        if (pos >= size) return Core::DecodeStatus::NeedMore;
        if (lenBytes >= kMaxRemainingLengthBytes) return Core::DecodeStatus::Malformed;
        const uint8_t b = data[pos++];
        ++lenBytes;
        remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
        if ((b & 0x80) == 0) break;
        multiplier *= 128;
    }
    if (remaining > kMaxRemainingLength) return Core::DecodeStatus::Malformed;

    total = 1 + lenBytes + static_cast<std::size_t>(remaining);
    if (size - offset < total) return Core::DecodeStatus::NeedMore;
    bodyOffset = pos;
    return Core::DecodeStatus::Complete;
}

} // namespace

std::vector<uint8_t> MqttCodec::BuildPacket(MqttPacketType type,
                                            uint8_t flags,
                                            const std::vector<uint8_t>& body) {
    std::vector<uint8_t> out;
    out.reserve(body.size() + 5);
    out.push_back(static_cast<uint8_t>(
        (static_cast<uint8_t>(type) << 4) | (flags & 0x0F)));
    AppendVarint(out, static_cast<uint32_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void MqttCodec::AppendVarint(std::vector<uint8_t>& out, uint32_t v) {
    if (v > kMaxRemainingLength) v = kMaxRemainingLength;
    do {
        uint8_t byte = static_cast<uint8_t>(v % 128);
        v /= 128;
        if (v > 0) byte |= 0x80;
        out.push_back(byte);
    } while (v > 0);
}

void MqttCodec::AppendString(std::vector<uint8_t>& out, const std::string& s) {
    // 长度字段为 2 字节, 超过 65535 的字符串在 MQTT 中本身非法 — 截断保护
    const std::size_t n = s.size() > 65535u ? 65535u : s.size();
    AppendU16(out, static_cast<uint16_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

std::vector<uint8_t> MqttCodec::EncodeConnect(const std::string& clientId,
                                              const std::string& username,
                                              const std::string& password,
                                              uint16_t keepAliveSec,
                                              bool cleanSession,
                                              const std::string& willTopic,
                                              const std::string& willPayload,
                                              int willQos,
                                              bool willRetain) {
    std::vector<uint8_t> body;

    // ── 可变头 ──
    AppendString(body, std::string("MQTT"));   // 协议名
    body.push_back(0x04);                      // 协议级别 4 = 3.1.1

    uint8_t connectFlags = cleanSession ? 0x02 : 0x00;
    const bool hasWill = !willTopic.empty();
    if (hasWill) {
        connectFlags |= 0x04;                                     // will flag
        connectFlags |= static_cast<uint8_t>((willQos & 0x03) << 3);
        if (willRetain) connectFlags |= 0x20;
    }
    if (!username.empty()) connectFlags |= 0x80;
    if (!password.empty()) connectFlags |= 0x40;
    body.push_back(connectFlags);

    AppendU16(body, keepAliveSec);

    // ── 载荷 (顺序固定: clientId, will topic, will payload, username, password) ──
    AppendString(body, clientId);
    if (hasWill) {
        AppendString(body, willTopic);
        AppendString(body, willPayload);
    }
    if (!username.empty()) AppendString(body, username);
    if (!password.empty()) AppendString(body, password);

    // CONNECT 固定头低 4 位保留为 0
    return BuildPacket(MqttPacketType::Connect, 0, body);
}

std::vector<uint8_t> MqttCodec::EncodePublish(const std::string& topic,
                                              const std::string& payload,
                                              int qos,
                                              bool retain,
                                              uint16_t packetId) {
    return EncodePublishBytes(topic,
                              reinterpret_cast<const uint8_t*>(payload.data()),
                              payload.size(), qos, retain, packetId);
}

std::vector<uint8_t> MqttCodec::EncodePublishBytes(const std::string& topic,
                                                   const uint8_t* payload,
                                                   std::size_t payloadSize,
                                                   int qos,
                                                   bool retain,
                                                   uint16_t packetId) {
    int q = qos < 0 ? 0 : (qos > 1 ? 1 : qos);   // 本模块只支持 QoS0/1
    std::vector<uint8_t> body;
    AppendString(body, topic);
    if (q > 0) AppendU16(body, packetId);
    if (payloadSize > 0 && payload != 0) {
        body.insert(body.end(), payload, payload + payloadSize);
    }

    uint8_t flags = static_cast<uint8_t>((q & 0x03) << 1);
    if (retain) flags |= 0x01;
    return BuildPacket(MqttPacketType::Publish, flags, body);
}

std::vector<uint8_t> MqttCodec::EncodePuback(uint16_t packetId) {
    std::vector<uint8_t> body;
    AppendU16(body, packetId);
    return BuildPacket(MqttPacketType::Puback, 0, body);
}

std::vector<uint8_t> MqttCodec::EncodeSubscribe(const std::string& topic,
                                                int qos,
                                                uint16_t packetId) {
    const int q = qos < 0 ? 0 : (qos > 1 ? 1 : qos);   // 本模块只支持 QoS0/1
    std::vector<uint8_t> body;
    AppendU16(body, packetId);
    AppendString(body, topic);
    body.push_back(static_cast<uint8_t>(q & 0x03));    // requested QoS
    return BuildPacket(MqttPacketType::Subscribe, 0x02, body);   // 固定头保留位必须为 0010
}

std::vector<uint8_t> MqttCodec::EncodePingreq() {
    return BuildPacket(MqttPacketType::Pingreq, 0, std::vector<uint8_t>());
}

std::vector<uint8_t> MqttCodec::EncodeDisconnect() {
    return BuildPacket(MqttPacketType::Disconnect, 0, std::vector<uint8_t>());
}

MqttDecodeResult MqttCodec::TryDecode(const std::vector<uint8_t>& buf,
                                      std::size_t offset,
                                      MqttPacket& out,
                                      std::size_t& consumed) {
    consumed = 0;
    uint8_t typeRaw = 0;
    uint8_t flags = 0;
    std::size_t bodyOffset = 0;
    std::size_t total = 0;
    const Core::DecodeStatus st = ParseFixedHeader(
        buf.empty() ? 0 : &buf[0], buf.size(), offset,
        typeRaw, flags, bodyOffset, total);
    if (st == Core::DecodeStatus::NeedMore) return MqttDecodeResult::NeedMore;
    if (st == Core::DecodeStatus::Malformed) return MqttDecodeResult::Malformed;

    out.type = static_cast<MqttPacketType>(typeRaw);
    out.flags = flags;
    out.body.assign(buf.begin() + static_cast<std::ptrdiff_t>(bodyOffset),
                    buf.begin() + static_cast<std::ptrdiff_t>(offset + total));
    consumed = total;
    return MqttDecodeResult::Complete;
}

bool MqttCodec::ParseConnack(const MqttPacket& p,
                             uint8_t& returnCode,
                             bool& sessionPresent) {
    if (p.type != MqttPacketType::Connack || p.body.size() != 2) return false;
    sessionPresent = (p.body[0] & 0x01) != 0;
    returnCode = p.body[1];
    return true;
}

bool MqttCodec::ParseSuback(const MqttPacket& p,
                            uint16_t& packetId,
                            std::vector<uint8_t>& returnCodes) {
    packetId = 0;
    returnCodes.clear();
    if (p.type != MqttPacketType::Suback || p.body.size() < 2) return false;
    packetId = static_cast<uint16_t>(
        (static_cast<uint16_t>(p.body[0]) << 8) | p.body[1]);
    returnCodes.assign(p.body.begin() + 2, p.body.end());
    return !returnCodes.empty();
}

bool MqttCodec::ParsePublish(const MqttPacket& p,
                             std::string& topic,
                             uint16_t& packetId,
                             std::size_t& payloadOffset) {
    packetId = 0;
    payloadOffset = 0;
    if (p.type != MqttPacketType::Publish || p.body.size() < 2) return false;

    const std::size_t topicLen =
        (static_cast<std::size_t>(p.body[0]) << 8) | p.body[1];
    std::size_t pos = 2 + topicLen;
    if (pos > p.body.size()) return false;
    topic.assign(reinterpret_cast<const char*>(&p.body[2]), topicLen);

    const uint8_t qos = static_cast<uint8_t>((p.flags >> 1) & 0x03);
    if (qos > 0) {
        if (pos + 2 > p.body.size()) return false;
        packetId = static_cast<uint16_t>(
            (static_cast<uint16_t>(p.body[pos]) << 8) | p.body[pos + 1]);
        pos += 2;
    }
    payloadOffset = pos;
    return true;
}

// ──────────────────── Core::IFrameCodec (ADR-0020) ────────────────────

Core::DecodeStatus MqttCodec::Decode(const Core::ByteView& buf,
                                     std::size_t& consumed,
                                     Core::Frame& out) {
    consumed = 0;
    uint8_t typeRaw = 0;
    uint8_t flags = 0;
    std::size_t bodyOffset = 0;
    std::size_t total = 0;
    const Core::DecodeStatus st = ParseFixedHeader(
        buf.data, buf.size, 0, typeRaw, flags, bodyOffset, total);
    if (st != Core::DecodeStatus::Complete) return st;

    out.Clear();
    out.type = typeRaw;
    out.flags = flags;

    const uint8_t* body = buf.data + bodyOffset;
    const std::size_t bodySize = total - bodyOffset;

    switch (typeRaw) {
    case 2: {   // CONNACK: [0]=sessionPresent, [1]=returnCode
        if (bodySize >= 2) {
            out.SetBool("sessionPresent", (body[0] & 0x01) != 0);
            out.SetUInt("returnCode", body[1]);
        }
        break;
    }
    case 3: {   // PUBLISH: topic + [packetId if qos>0] + payload
        if (bodySize < 2) break;
        const std::size_t topicLen = ReadU16(body);
        std::size_t pos = 2 + topicLen;
        if (pos > bodySize) break;
        out.SetString("topic", std::string(
            reinterpret_cast<const char*>(body + 2), topicLen));
        const uint8_t qos = static_cast<uint8_t>((flags >> 1) & 0x03);
        out.SetUInt("qos", qos);
        out.SetBool("retain", (flags & 0x01) != 0);
        if (qos > 0) {
            if (pos + 2 > bodySize) break;
            out.SetUInt("packetId", ReadU16(body + pos));
            pos += 2;
        }
        out.payload = Core::ByteView(body + pos, bodySize - pos);
        break;
    }
    case 4:     // PUBACK
        if (bodySize >= 2) out.SetUInt("packetId", ReadU16(body));
        break;
    case 9: {   // SUBACK: packetId + per-topic return codes
        if (bodySize >= 2) {
            out.SetUInt("packetId", ReadU16(body));
            out.SetBytes("returnCodes", Core::ByteView(body + 2, bodySize - 2));
        }
        break;
    }
    default:
        break;
    }

    consumed = total;
    return Core::DecodeStatus::Complete;
}

bool MqttCodec::Encode(const Core::Frame& frame, std::vector<uint8_t>& out) {
    out.clear();
    uint64_t u = 0;

    switch (frame.type) {
    case 1: {   // CONNECT
        std::string clientId, username, password, willTopic, willPayload;
        bool cleanSession = true;
        bool willRetain = false;
        uint64_t willQos = 0;
        frame.GetString("clientId", clientId);
        frame.GetString("username", username);
        frame.GetString("password", password);
        frame.GetUInt("keepAliveSec", u);
        frame.GetBool("cleanSession", cleanSession);
        frame.GetString("willTopic", willTopic);
        frame.GetString("willPayload", willPayload);
        frame.GetUInt("willQos", willQos);
        frame.GetBool("willRetain", willRetain);
        out = EncodeConnect(clientId, username, password,
                            static_cast<uint16_t>(u), cleanSession,
                            willTopic, willPayload,
                            static_cast<int>(willQos), willRetain);
        return true;
    }
    case 3: {   // PUBLISH
        std::string topic;
        uint64_t qos = 0;
        uint64_t packetId = 0;
        bool retain = false;
        frame.GetString("topic", topic);
        frame.GetUInt("qos", qos);
        frame.GetUInt("packetId", packetId);
        frame.GetBool("retain", retain);
        out = EncodePublishBytes(topic, frame.payload.data, frame.payload.size,
                                 static_cast<int>(qos), retain,
                                 static_cast<uint16_t>(packetId));
        return true;
    }
    case 4:     // PUBACK
        frame.GetUInt("packetId", u);
        out = EncodePuback(static_cast<uint16_t>(u));
        return true;
    case 8: {   // SUBSCRIBE
        std::string topic;
        uint64_t qos = 0;
        uint64_t packetId = 0;
        frame.GetString("topic", topic);
        frame.GetUInt("qos", qos);
        frame.GetUInt("packetId", packetId);
        out = EncodeSubscribe(topic, static_cast<int>(qos),
                              static_cast<uint16_t>(packetId));
        return true;
    }
    case 12:    // PINGREQ
        out = EncodePingreq();
        return true;
    case 14:    // DISCONNECT
        out = EncodeDisconnect();
        return true;
    default:
        return false;
    }
}

}} // namespace MyProt::Northbound

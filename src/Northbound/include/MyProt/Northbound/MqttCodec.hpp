// src/Northbound/include/MyProt/Northbound/MqttCodec.hpp
// MQTT 3.1.1 报文编解码 — 仅覆盖发布/订阅所需子集 (C++11, ADR-0010)
// 支持: CONNECT / CONNACK / PUBLISH(QoS0,1) / PUBACK / SUBSCRIBE / SUBACK /
//       PINGREQ / PINGRESP / DISCONNECT
// 不支持: 取消订阅 / QoS2 / TLS (见 docs/modules/11_Northbound.md)
//
// 本类是帧层 (Core::IFrameCodec) 的 MQTT 实现：Decode/Encode 在字节与
// Core::Frame 之间转换，不含任何会话状态 (ADR-0020)。
// 原有的静态编码/解析函数保留，供类型安全的调用点与既有单测直接使用。

#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "MyProt/Core/FrameCodec.hpp"

namespace MyProt { namespace Northbound {

/// 报文类型 (MQTT 3.1.1 §2.2.1, 高 4 位)
enum class MqttPacketType {
    Connect    = 1,
    Connack    = 2,
    Publish    = 3,
    Puback     = 4,
    Subscribe  = 8,
    Suback     = 9,
    Pingreq    = 12,
    Pingresp   = 13,
    Disconnect = 14
};

/// 解码结果 — 流式解析需要区分"还没收全"与"报文非法"
enum class MqttDecodeResult {
    Complete,   // out / consumed 有效
    NeedMore,   // 剩余长度或报文体未收全, 继续读
    Malformed   // 非法报文 (长度溢出/保留位错误), 应断开重连
};

/// 一个完整报文 — 固定头类型 + 标志位 + 报文体 (不含固定头)
struct MqttPacket {
    MqttPacketType type;
    uint8_t flags;              // 固定头低 4 位
    std::vector<uint8_t> body;

    MqttPacket() : type(MqttPacketType::Connack), flags(0) {}
};

/// 无状态编解码工具 — 全部为静态方法, 便于单测直接比对字节流
class MqttCodec : public Core::IFrameCodec {
public:
    // ── 通用帧接口 (Core::IFrameCodec, ADR-0020) ──

    /// 从字节流切出一个报文并填充 Core::Frame。
    /// 填充的字段: CONNACK{sessionPresent,returnCode}、PUBLISH{topic,qos,retain,packetId}、
    ///             SUBACK{packetId,returnCodes}、PUBACK{packetId};
    /// frame.payload 指向 buf 内的应用载荷 (PUBLISH) 或为空。
    Core::DecodeStatus Decode(const Core::ByteView& buf,
                              std::size_t& consumed,
                              Core::Frame& out);

    /// 按 frame.type + 字段生成报文; 不支持的类型返回 false。
    bool Encode(const Core::Frame& frame, std::vector<uint8_t>& out);

    // ── 编码 ──

    /// CONNECT (clientId 必填; username/password 为空则不设置对应标志位)
    static std::vector<uint8_t> EncodeConnect(const std::string& clientId,
                                              const std::string& username,
                                              const std::string& password,
                                              uint16_t keepAliveSec,
                                              bool cleanSession,
                                              const std::string& willTopic,
                                              const std::string& willPayload,
                                              int willQos,
                                              bool willRetain);

    /// PUBLISH (qos=0 时 packetId 被忽略; retain 置固定头 bit0)
    static std::vector<uint8_t> EncodePublish(const std::string& topic,
                                              const std::string& payload,
                                              int qos,
                                              bool retain,
                                              uint16_t packetId);

    static std::vector<uint8_t> EncodePuback(uint16_t packetId);

    /// SUBSCRIBE (单主题; 固定头 flags 必须为 0b0010)
    static std::vector<uint8_t> EncodeSubscribe(const std::string& topic,
                                                int qos,
                                                uint16_t packetId);
    static std::vector<uint8_t> EncodePingreq();
    static std::vector<uint8_t> EncodeDisconnect();

    // ── 解码 ──

    /// 从 buf[offset] 起解析一个报文。
    /// @param consumed 成功时写入该报文占用的总字节数 (固定头 + 报文体)
    static MqttDecodeResult TryDecode(const std::vector<uint8_t>& buf,
                                      std::size_t offset,
                                      MqttPacket& out,
                                      std::size_t& consumed);

    /// CONNACK 报文体: [0]=sessionPresent 标志, [1]=返回码
    static bool ParseConnack(const MqttPacket& p,
                             uint8_t& returnCode,
                             bool& sessionPresent);

    /// SUBACK 报文体拆解: packetId + 逐主题返回码 (0/1/2 = 成功, 0x80 = 失败)
    static bool ParseSuback(const MqttPacket& p,
                            uint16_t& packetId,
                            std::vector<uint8_t>& returnCodes);

    /// PUBLISH 报文体拆解: topic / packetId (QoS0 为 0) / payload 起始偏移
    static bool ParsePublish(const MqttPacket& p,
                             std::string& topic,
                             uint16_t& packetId,
                             std::size_t& payloadOffset);

    /// 剩余长度变长整数编码 (MQTT 3.1.1 §2.2.3)
    static void AppendVarint(std::vector<uint8_t>& out, uint32_t v);

    /// UTF-8 字符串: 2 字节大端长度 + 内容 (§1.5.3)
    static void AppendString(std::vector<uint8_t>& out, const std::string& s);

private:
    static std::vector<uint8_t> BuildPacket(MqttPacketType type,
                                            uint8_t flags,
                                            const std::vector<uint8_t>& body);

    static std::vector<uint8_t> EncodePublishBytes(const std::string& topic,
                                                   const uint8_t* payload,
                                                   std::size_t payloadSize,
                                                   int qos,
                                                   bool retain,
                                                   uint16_t packetId);
};

}} // namespace MyProt::Northbound

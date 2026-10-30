// src/Northbound/src/MqttSession.cpp
// MQTT 3.1.1 会话语义实现 — 纯状态机, 不持有 socket / 定时器 (ADR-0020)

#include "MyProt/Northbound/MqttSession.hpp"

#include "MyProt/Core/Log.hpp"
#include "MyProt/Northbound/MqttCodec.hpp"

namespace MyProt { namespace Northbound {

namespace {

const char* ConnackReason(uint8_t code) {
    switch (code) {
    case 1: return "协议版本不被接受";
    case 2: return "clientId 被拒绝";
    case 3: return "服务端不可用";
    case 4: return "用户名或密码错误";
    case 5: return "未授权";
    default: return "未知错误";
    }
}

// 报文类型 ID — case 标签需要常量表达式, 因此不用函数转换
const uint32_t kPktConnect   = static_cast<uint32_t>(MqttPacketType::Connect);
const uint32_t kPktConnack   = static_cast<uint32_t>(MqttPacketType::Connack);
const uint32_t kPktPublish   = static_cast<uint32_t>(MqttPacketType::Publish);
const uint32_t kPktPuback    = static_cast<uint32_t>(MqttPacketType::Puback);
const uint32_t kPktSubscribe = static_cast<uint32_t>(MqttPacketType::Subscribe);
const uint32_t kPktSuback    = static_cast<uint32_t>(MqttPacketType::Suback);
const uint32_t kPktPingreq   = static_cast<uint32_t>(MqttPacketType::Pingreq);
const uint32_t kPktPingresp  = static_cast<uint32_t>(MqttPacketType::Pingresp);

} // namespace

MqttSession::MqttSession(Core::IFrameCodec& codec, const Core::MqttConfig& cfg)
    : _codec(codec)
    , _cfg(cfg)
    , _cleanSession(true)
    , _willQos(0)
    , _willRetain(false)
    , _nextPacketId(1) {}

void MqttSession::SetCleanSession(bool clean) {
    _cleanSession = clean;
}

void MqttSession::SetWill(const std::string& willTopic,
                          const std::string& willPayload,
                          int willQos,
                          bool willRetain) {
    _willTopic = willTopic;
    _willPayload = willPayload;
    _willQos = willQos;
    _willRetain = willRetain;
}

void MqttSession::SetOnMessage(MessageHandler handler) {
    _onMessage = handler;
}

void MqttSession::SetOnMessageEx(MessageHandlerEx handler) {
    _onMessageEx = handler;
}

void MqttSession::SetSendHandler(SendHandler handler) {
    _send = handler;
}

uint16_t MqttSession::NextPacketId() {
    const uint16_t id = _nextPacketId++;
    if (_nextPacketId == 0) _nextPacketId = 1;   // 0 保留
    return id;
}

// ──────────────────── outbound ────────────────────

void MqttSession::BuildConnect(std::vector<uint8_t>& out) {
    Core::Frame frame;
    frame.type = kPktConnect;
    frame.SetString("clientId", _cfg.clientId);
    frame.SetString("username", _cfg.username);
    frame.SetString("password", _cfg.password);
    frame.SetUInt("keepAliveSec",
                  static_cast<uint64_t>(_cfg.keepAliveSec > 0 ? _cfg.keepAliveSec : 0));
    frame.SetBool("cleanSession", _cleanSession);
    frame.SetString("willTopic", _willTopic);
    frame.SetString("willPayload", _willPayload);
    frame.SetUInt("willQos", static_cast<uint64_t>(_willQos < 0 ? 0
                         : (_willQos > 2 ? 2 : _willQos)));
    frame.SetBool("willRetain", _willRetain);
    _codec.Encode(frame, out);
}

bool MqttSession::BuildPublish(const std::string& topic,
                               const std::string& payload,
                               int qos,
                               bool retain,
                               std::vector<uint8_t>& out) {
    const int q = qos < 0 ? 0 : (qos > 1 ? 1 : qos);   // 本模块只支持 QoS0/1

    Core::Frame frame;
    frame.type = kPktPublish;
    frame.SetString("topic", topic);
    frame.SetUInt("qos", static_cast<uint64_t>(q));
    frame.SetBool("retain", retain);
    if (q > 0) frame.SetUInt("packetId", NextPacketId());
    frame.payload = Core::ByteView(
        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    return _codec.Encode(frame, out);
}

void MqttSession::BuildSubscribe(const std::string& topic, int qos,
                                 std::vector<uint8_t>& out) {
    const int q = qos < 0 ? 0 : (qos > 1 ? 1 : qos);   // 本模块只支持 QoS0/1

    Core::Frame frame;
    frame.type = kPktSubscribe;
    frame.SetString("topic", topic);
    frame.SetUInt("qos", static_cast<uint64_t>(q));
    frame.SetUInt("packetId", NextPacketId());
    _codec.Encode(frame, out);
}

void MqttSession::BuildPingreq(std::vector<uint8_t>& out) {
    Core::Frame frame;
    frame.type = kPktPingreq;
    _codec.Encode(frame, out);
}

void MqttSession::BuildPuback(uint16_t packetId, std::vector<uint8_t>& out) {
    Core::Frame frame;
    frame.type = kPktPuback;
    frame.SetUInt("packetId", packetId);
    _codec.Encode(frame, out);
}

// ──────────────────── subscriptions ────────────────────

void MqttSession::AddSubscription(const std::string& topic, int qos) {
    for (std::size_t i = 0; i < _subscriptions.size(); ++i) {
        if (_subscriptions[i].topic == topic) {
            _subscriptions[i].qos = qos;
            return;
        }
    }
    _subscriptions.push_back(Subscription(topic, qos));
}

std::vector<std::vector<uint8_t> > MqttSession::BuildSubscriptionPackets() {
    std::vector<std::vector<uint8_t> > packets;
    packets.reserve(_subscriptions.size());
    for (std::size_t i = 0; i < _subscriptions.size(); ++i) {
        std::vector<uint8_t> packet;
        BuildSubscribe(_subscriptions[i].topic, _subscriptions[i].qos, packet);
        packets.push_back(packet);
    }
    return packets;
}

// ──────────────────── inbound ────────────────────

MqttSession::Event MqttSession::OnFrame(const Core::Frame& frame) {
    switch (frame.type) {
    case kPktConnack: {
        bool sessionPresent = false;
        uint64_t returnCode = 0;
        if (!frame.GetBool("sessionPresent", sessionPresent)
                || !frame.GetUInt("returnCode", returnCode)) {
            _lastError = "invalid CONNACK";
            return Event::Fatal;
        }
        if (returnCode != 0) {
            _lastError = std::string("CONNACK 拒绝: ")
                + ConnackReason(static_cast<uint8_t>(returnCode));
            return Event::Fatal;
        }
        _lastError.clear();
        return Event::Connected;
    }
    case kPktSuback: {
        uint64_t packetId = 0;
        Core::ByteView codes;
        if (frame.GetUInt("packetId", packetId)
                && frame.GetBytes("returnCodes", codes)) {
            for (std::size_t i = 0; i < codes.size; ++i) {
                if (codes[i] == 0x80) {
                    LOG_WARN("MQTT", "订阅被拒绝 (packetId=%u)",
                             static_cast<unsigned>(packetId));
                } else {
                    LOG_INFO("MQTT", "订阅成功 (packetId=%u, qos=%u)",
                             static_cast<unsigned>(packetId),
                             static_cast<unsigned>(codes[i]));
                }
            }
        }
        return Event::None;
    }
    case kPktPuback:
        // QoS1 确认 — 当前无重传队列, 仅消费报文避免 rx 缓冲堆积
        return Event::None;

    case kPktPingresp:
        return Event::None;

    case kPktPublish: {
        std::string topic;
        uint64_t qos = 0;
        uint64_t packetId = 0;
        bool retain = false;
        if (!frame.GetString("topic", topic)) return Event::None;
        frame.GetUInt("qos", qos);
        frame.GetBool("retain", retain);

        if (qos == 1 && frame.GetUInt("packetId", packetId) && packetId != 0 && _send) {
            std::vector<uint8_t> puback;
            BuildPuback(static_cast<uint16_t>(packetId), puback);
            _send(puback);
        }
        const char* payload = frame.payload.size > 0
            ? reinterpret_cast<const char*>(frame.payload.data) : "";
        const std::string payloadText(payload, frame.payload.size);
        if (_onMessage) _onMessage(topic, payloadText);
        if (_onMessageEx) _onMessageEx(topic, payloadText,
                                      static_cast<int>(qos), retain);
        return Event::None;
    }
    default:
        return Event::None;
    }
}

}} // namespace MyProt::Northbound

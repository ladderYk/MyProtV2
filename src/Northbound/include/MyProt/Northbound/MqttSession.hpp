// src/Northbound/include/MyProt/Northbound/MqttSession.hpp
// MQTT 3.1.1 session semantics — handshake, subscriptions, acknowledgements and keepalive
// packets. No socket, no timer, no byte layout: all wire encoding goes through
// Core::IFrameCodec (ADR-0020).
//
// Split of responsibilities:
//   MqttCodec   : bytes <-> Core::Frame          (wire format)
//   MqttSession : Core::Frame -> protocol action (state machine)
//   MqttClient  : asio I/O, timers, tx queue     (transport)
//
// C++11 / VS2015 (ADR-0010).

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "MyProt/Core/FrameCodec.hpp"
#include "MyProt/Core/MqttConfig.hpp"

namespace MyProt { namespace Northbound {

class MqttSession {
public:
    enum class Event {
        None,        // frame consumed; nothing for the transport to do
        Connected,   // CONNACK accepted; transport should mark connected, arm keepalive,
                     // then send BuildSubscriptionPackets() and flush its own backlog
        Fatal        // protocol violation; transport must drop the connection
    };

    /// 收到 PUBLISH 时回调 (topic, payload); payload 已从帧视图拷贝为 string
    typedef std::function<void(const std::string&, const std::string&)> MessageHandler;
    /// 扩展回调 (topic, payload, qos, retain) — 观测面板需要展示 QoS/retain;
    /// 与 MessageHandler 互不替代, 两者可同时注册。
    typedef std::function<void(const std::string&, const std::string&, int, bool)>
        MessageHandlerEx;
    /// 会话需要立即发出的报文 (目前只用于 QoS1 PUBACK); 由 transport 入发送队列
    typedef std::function<void(const std::vector<uint8_t>&)> SendHandler;

    MqttSession(Core::IFrameCodec& codec, const Core::MqttConfig& cfg);

    void SetCleanSession(bool clean);
    void SetWill(const std::string& willTopic,
                 const std::string& willPayload,
                 int willQos,
                 bool willRetain);
    void SetOnMessage(MessageHandler handler);
    void SetOnMessageEx(MessageHandlerEx handler);
    void SetSendHandler(SendHandler handler);

    // ── outbound ──

    void BuildConnect(std::vector<uint8_t>& out);
    bool BuildPublish(const std::string& topic,
                      const std::string& payload,
                      int qos,
                      bool retain,
                      std::vector<uint8_t>& out);
    void BuildSubscribe(const std::string& topic, int qos,
                        std::vector<uint8_t>& out);
    void BuildPingreq(std::vector<uint8_t>& out);

    // ── subscriptions (bookkeeping only) ──

    /// Registers or updates a subscription; the caller decides whether to send SUBSCRIBE now.
    void AddSubscription(const std::string& topic, int qos);

    /// One SUBSCRIBE per registered topic, used after every (re)connect.
    std::vector<std::vector<uint8_t> > BuildSubscriptionPackets();

    // ── inbound ──

    /// Applies one decoded frame. Returns Fatal on a protocol violation; LastError() then
    /// carries the reason for logs / LastError().
    Event OnFrame(const Core::Frame& frame);

    const std::string& LastError() const { return _lastError; }

private:
    struct Subscription {
        std::string topic;
        int qos;
        Subscription(const std::string& t, int q) : topic(t), qos(q) {}
    };

    uint16_t NextPacketId();
    void BuildPuback(uint16_t packetId, std::vector<uint8_t>& out);

    Core::IFrameCodec& _codec;
    Core::MqttConfig _cfg;

    bool _cleanSession;
    std::string _willTopic;
    std::string _willPayload;
    int _willQos;
    bool _willRetain;

    uint16_t _nextPacketId;
    std::vector<Subscription> _subscriptions;
    MessageHandler _onMessage;
    MessageHandlerEx _onMessageEx;
    SendHandler _send;
    std::string _lastError;

    MqttSession(const MqttSession&);
    MqttSession& operator=(const MqttSession&);
};

}} // namespace MyProt::Northbound

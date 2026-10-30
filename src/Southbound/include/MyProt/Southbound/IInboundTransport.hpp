// src/Southbound/include/MyProt/Southbound/IInboundTransport.hpp
// Inbound (device-initiated) transport - the gateway is the receiver (C++11, ADR-0010).
// Deliberately separate from Transport::IChannel: IChannel is request/response only,
// forcing a subscription or a listener into it would corrupt the transaction semantics.

#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace MyProt { namespace Southbound {

/// One inbound message as delivered by an adapter.
struct InboundFrame {
    std::string topic;          // MQTT topic; empty for TCP listeners
    std::string payload;        // raw payload bytes (JSON in v1)
    int64_t timestampMs;        // local receive time (epoch ms)

    InboundFrame() : timestampMs(0) {}
};

/// Inbound adapters: MQTT subscribe / TCP listen.
class IInboundTransport {
public:
    typedef std::function<void(const InboundFrame&)> FrameHandler;
    typedef std::function<void(const std::string&)> ErrorHandler;

    virtual ~IInboundTransport() {}

    /// Start receiving. Handlers run on the provided io_context thread.
    virtual void Start(FrameHandler onFrame, ErrorHandler onError) = 0;

    /// Stop and release all resources (subscriptions / listen port / sockets).
    virtual void Stop() = 0;

    virtual bool IsRunning() const = 0;
};

}} // namespace MyProt::Southbound

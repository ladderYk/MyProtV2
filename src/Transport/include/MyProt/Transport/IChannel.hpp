// src/Transport/include/MyProt/Transport/IChannel.hpp
// Channel interface - async callback model (C++11, ADR-0010 §2)
// Signature convention: trailing callback; the callback fires on the io_context thread, and implementers must keep it alive (shared_ptr capture)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Transport {

using Bytes = Core::Bytes;   // std::vector<uint8_t>

/// Connection-completion callback (invoked exactly once on success or failure)
using ConnectHandler = std::function<void(Core::Expected<void>)>;

/// Request-response completion callback (invoked exactly once when a full response frame is received or on failure)
using ReceiveHandler = std::function<void(Core::Expected<Core::Bytes>)>;

/// One-way send completion callback (invoked exactly once).
/// Success only means the request bytes were handed to the transport; it does NOT mean the
/// device received or processed them (a device that never answers gives no confirmation).
using SendHandler = std::function<void(Core::Expected<void>)>;

/// Channel abstract base class - the unified interface for the concrete Tcp/Tls/Serial channels.
/// One channel, one request: only one in-flight SendReceive at a time (the gateway layer serializes per device).
class IChannel {
public:
    virtual ~IChannel() = default;

    /// Asynchronously establish the physical connection; if not done by the timeout, fail as Timeout
    virtual void Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) = 0;

    /// Asynchronous request-response: send request, collect the full response frame per framingConfig rules;
    /// when framingConfig is null, use the channel's built-in default framing
    virtual void SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                             ReceiveHandler handler) = 0;

    /// One-way send: transmit the request bytes and complete immediately - no read, no framing,
    /// no waiting for a response. Use only when the device really does not answer (e.g. a Modbus
    /// broadcast); if it does answer, the unread bytes stay in the channel buffer and pollute the
    /// next request on the same channel.
    /// Default implementation fails as NotImplemented - only channels that support this semantic override it.
    virtual void Send(const Bytes& request,
                      std::chrono::milliseconds timeout,
                      SendHandler handler) {
        (void)request;
        (void)timeout;
        handler(Core::Unexpected(Core::Error::Code::NotImplemented,
                                 "one-way send not supported by this channel"));
    }

    /// Disconnect and release underlying resources; pending operations complete with ConnectionClosed
    virtual void Disconnect() = 0;

    virtual bool IsConnected() const = 0;

    /// A snapshot of the most recent failure's error (thread-safe)
    virtual Core::Error GetLastError() const = 0;
};

/// Channel smart pointer
using IChannelPtr = std::shared_ptr<IChannel>;

}} // namespace MyProt::Transport

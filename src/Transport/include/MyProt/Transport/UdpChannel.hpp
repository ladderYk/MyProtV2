// src/Transport/include/MyProt/Transport/UdpChannel.hpp
// UDP datagram channel - connectionless, one datagram == one frame (ADR-0002).
//
// Unlike TcpChannel there is no byte stream to split: a single async_receive_from
// yields the whole response. The framing config is used only as a maxFrameSize
// guard for the receive buffer, never for in-stream frame boundary detection.
// Timeouts and transport errors close the socket and clear _connected, forcing
// ChannelManager to recreate the channel on the next poll (stale late datagrams
// are discarded by the socket close, mirroring the TCP reconnect-on-timeout model).

#pragma once
#include "IChannel.hpp"
#include <asio.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace MyProt { namespace Transport {

/// UDP channel - based on asio::ip::udp::socket (datagram, no connect)
class UdpChannel : public IChannel,
                   public std::enable_shared_from_this<UdpChannel> {
public:
    UdpChannel(asio::io_context& io, const std::string& channelId);
    virtual ~UdpChannel();

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;

    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;

    /// One-way send: emit the datagram and complete without reading (broadcast support)
    void Send(const Bytes& request,
              std::chrono::milliseconds timeout,
              SendHandler handler) override;

    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// Request-response operation state (send_to then receive_from one datagram)
    struct RxOp {
        explicit RxOp(ReceiveHandler h)
            : handler(std::move(h)), done(false), maxFrameSize(1024) {}
        ReceiveHandler handler;
        std::shared_ptr<asio::steady_timer> watchdog;
        size_t maxFrameSize;
        bool done;
    };

    /// One-way send operation state
    struct TxOp {
        explicit TxOp(SendHandler h)
            : handler(std::move(h)), done(false) {}
        SendHandler handler;
        std::shared_ptr<asio::steady_timer> watchdog;
        bool done;
    };

    /// Receive exactly one datagram; the datagram length is the response length
    void ReceiveOne(const std::shared_ptr<RxOp>& op);

    /// Record the most recent error (thread-safe)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    std::string _channelId;
    asio::ip::udp::socket _socket;
    asio::ip::udp::resolver _resolver;
    asio::ip::udp::endpoint _peer;
    std::atomic<bool> _connected;
    // One-channel-one-request guard: only one in-flight SendReceive at a time
    std::atomic<bool> _inFlight;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;

    // Disallow copying
    UdpChannel(const UdpChannel&) = delete;
    UdpChannel& operator=(const UdpChannel&) = delete;
};

}} // namespace MyProt::Transport

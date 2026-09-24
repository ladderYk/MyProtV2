// src/Transport/include/MyProt/Transport/TcpChannel.hpp
// TCP channel - asio async callback model (C++11, ADR-0010 §2)

#pragma once
#include "IChannel.hpp"
#include <asio.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace MyProt { namespace Transport {

/// TCP channel - based on asio::ip::tcp::socket
/// Timeouts are implemented by a steady_timer watchdog: on expiry it closes the socket, and pending operations end with operation_aborted
class TcpChannel : public IChannel,
                   public std::enable_shared_from_this<TcpChannel> {
public:
    TcpChannel(asio::io_context& io, const std::string& channelId);
    virtual ~TcpChannel();

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;

    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;

    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// Stream-receive operation state (shared keep-alive across the callback chain)
    struct RxOp {
        explicit RxOp(ReceiveHandler h)
            : handler(std::move(h)), done(false), maxFrameSize(1024) {}
        ReceiveHandler handler;
        Core::Bytes buffer;
        std::shared_ptr<const Core::FramingConfig> framing;
        std::shared_ptr<asio::steady_timer> watchdog;
        size_t maxFrameSize;
        bool done;
    };

    /// Append-read a block of data; after each arrival judge completeness per the framing rules
    void ReadChunk(const std::shared_ptr<RxOp>& op);

    /// Record the most recent error (thread-safe)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    std::string _channelId;
    std::unique_ptr<asio::ip::tcp::socket> _socket;
    asio::ip::tcp::resolver _resolver;
    std::atomic<bool> _connected;
    // One-channel-one-request guard: only one in-flight SendReceive at a time
    // (the socket does not support concurrent read/write; a second one fails fast as Busy)
    std::atomic<bool> _inFlight;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;

    // Note: _lastEndpoint / _lastConnectTimeout were withdrawn (2026-08-31).
    // The original "borrow one re-send" inside SendReceive would form a double Connect storm with
    //        ChannelManager's GetOrCreateChannel; reconnection is ChannelManager's responsibility, this class does not cache.

    // Disallow copying
    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;
};

}} // namespace MyProt::Transport

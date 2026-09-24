// src/Transport/include/MyProt/Transport/SerialChannel.hpp
// Serial channel - async callback model + silence framing (C++11, ADR-0010 §2)

#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include <asio.hpp>
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Transport {

/// Serial channel - based on asio::serial_port
/// Silence framing: a character gap exceeding frameGap marks the end of a frame (the Modbus RTU 3.5-character convention)
class SerialChannel : public IChannel,
                      public std::enable_shared_from_this<SerialChannel> {
public:
    SerialChannel(asio::io_context& io,
                  const Core::SerialTransportConfig& serialConfig,
                  const std::string& channelId);

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
    /// Silent-framing stream-receive operation state (shared keep-alive across the callback chain)
    struct OpState;

    /// Append-read a block of characters; each arrival restarts the silence timer
    void ReadSome(const std::shared_ptr<OpState>& op,
                  const std::shared_ptr<asio::serial_port>& port);

    /// Record the most recent error (thread-safe)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    Core::SerialTransportConfig _serial;
    std::string _channelId;
    std::shared_ptr<asio::serial_port> _port;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;

    // Disallow copying
    SerialChannel(const SerialChannel&) = delete;
    SerialChannel& operator=(const SerialChannel&) = delete;
};

}} // namespace MyProt::Transport

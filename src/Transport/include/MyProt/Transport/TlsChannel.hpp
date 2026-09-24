// src/Transport/include/MyProt/Transport/TlsChannel.hpp
// TLS channel - async callback model (ADR-0010 §2); a v1 placeholder, signature already frozen

#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <asio.hpp>
#include "MyProt/Transport/IChannel.hpp"

namespace MyProt { namespace Transport {

/// TLS channel - based on asio::ssl::stream<asio::ip::tcp::socket> (to be implemented)
/// The async model matches TcpChannel (ADR-0010 §2)
class TlsChannel : public IChannel,
                   public std::enable_shared_from_this<TlsChannel> {
public:
    TlsChannel(asio::io_context& io,
               const Core::TlsTransportConfig& tlsConfig,
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
    asio::io_context& _io;
    Core::TlsTransportConfig _tls;
    std::string _channelId;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport

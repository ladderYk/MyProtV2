// src/Transport/include/MyProt/Transport/TlsChannel.hpp
// TLS 通道 — 异步回调模型 (ADR-0010 §2); v1 占位, 签名已冻结

#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <atomic>
#include <asio.hpp>
#include "MyProt/Transport/IChannel.hpp"

namespace MyProt { namespace Transport {

/// TLS 通道 — 基于 asio::ssl::stream<asio::ip::tcp::socket> (待实现)
/// 异步模型同 TcpChannel (ADR-0010 §2)
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

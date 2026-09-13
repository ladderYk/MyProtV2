// src/Transport/src/TlsChannel.cpp — TLS 通道占位 (用户决策: v1 暂不实现)
// 签名已冻结; Connect/SendReceive 返回 NotImplemented

#include "MyProt/Transport/TlsChannel.hpp"

namespace MyProt { namespace Transport {

TlsChannel::TlsChannel(asio::io_context& io,
                       const Core::TlsTransportConfig& tlsConfig,
                       const std::string& channelId)
    : _io(io)
    , _tls(tlsConfig)
    , _channelId(channelId)
    , _connected(false) {}

void TlsChannel::Connect(const Core::ConnectionConfig& /*endpoint*/,
                         std::chrono::milliseconds /*timeout*/,
                         ConnectHandler handler) {
    handler(Core::Unexpected(Core::Error::Code::NotImplemented,
                             "TlsChannel::Connect not yet implemented"));
}

void TlsChannel::SendReceive(const Bytes& /*request*/,
                             std::shared_ptr<const Core::FramingConfig> /*framingConfig*/,
                             std::chrono::milliseconds /*timeout*/,
                             ReceiveHandler handler) {
    handler(Core::Unexpected(Core::Error::Code::NotImplemented,
                             "TlsChannel::SendReceive not yet implemented"));
}

void TlsChannel::Disconnect() { _connected.store(false); }

bool TlsChannel::IsConnected() const { return _connected.load(); }

Core::Error TlsChannel::GetLastError() const {
    std::lock_guard<std::mutex> lock(_errorMutex);
    return _lastError;
}

}} // namespace MyProt::Transport

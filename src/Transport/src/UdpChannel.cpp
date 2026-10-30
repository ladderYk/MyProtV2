// src/Transport/src/UdpChannel.cpp
// UDP 数据报通道实现 — asio 异步回调模型 (C++11, ADR-0002 / ADR-0010 §2)
// 无连接: Connect 仅打开 socket + 解析对端; 一个数据报 = 一帧 (不做流内切帧)。
// 注意: 工程定义 ASIO_NO_DEPRECATED, 仅用新版 API (async_resolve(host,port) / asio::post)

#include "MyProt/Transport/UdpChannel.hpp"
#include "MyProt/Core/Log.hpp"
#include <asio/post.hpp>
#include <asio/error.hpp>
#include <utility>

namespace MyProt { namespace Transport {

namespace {

/// 取当前成帧分支的帧长上限 (仅用于接收缓冲大小保护; UDP 不据此切帧)
size_t MaxFrameSizeOf(const Core::FramingConfig& f) {
    switch (f.type) {
    case Core::FramingType::Fixed:   return static_cast<size_t>(f.fixed.fixedLength);
    case Core::FramingType::Silence: return static_cast<size_t>(f.silence.maxFrameSize);
    default:                         return static_cast<size_t>(f.lengthField.maxFrameSize);
    }
}

/// UDP 数据报理论上限 64KB; 接收缓冲 = min(配置上限, 65535), 至少给一个合理下限
size_t ReceiveCapOf(size_t maxFrameSize) {
    if (maxFrameSize == 0) return 1472;         // 常见单帧 MTU 载荷兜底
    return maxFrameSize > 65535 ? 65535 : maxFrameSize;
}

} // namespace

UdpChannel::UdpChannel(asio::io_context& io, const std::string& channelId)
    : _io(io)
    , _channelId(channelId)
    , _socket(io)
    , _resolver(io)
    , _connected(false)
    , _inFlight(false) {}

UdpChannel::~UdpChannel() {
    Disconnect();
}

void UdpChannel::Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) {
    if (_connected.load()) {
        // 已就绪 → 直接成功 (数据报"连接"即 socket 打开, 复用缓存)
        asio::post(_io, [handler]() { handler(Core::Expected<void>()); });
        return;
    }
    if (endpoint.host.empty()) {
        SetError(Core::Error::Code::ConnectionRefused, "udp connect: empty host");
        asio::post(_io, [handler]() {
            handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, "empty host"));
        });
        return;
    }
    // port=0 的有效端口解析在 ChannelManager::PerformConnect (协议 defaultPort, ADR-0012 附录A.1)。
    if (endpoint.port == 0) {
        SetError(Core::Error::Code::ConnectionRefused, "udp connect: no port");
        asio::post(_io, [handler]() {
            handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, "no port"));
        });
        return;
    }

    LOG_INFO("Channel", "UDP 就绪开始: %s:%u (timeout=%lldms, channel=%s)",
             endpoint.host.c_str(), (unsigned)endpoint.port,
             (long long)timeout.count(), _channelId.c_str());

    auto self = shared_from_this();

    // 解析看门狗: 到期取消挂起的 resolve (数据报无 connect, 仅 DNS 可能阻塞)
    auto timer = std::make_shared<asio::steady_timer>(_io);
    timer->expires_after(timeout);
    timer->async_wait([self](const asio::error_code& ec) {
        if (!ec && !self->_connected.load()) {
            self->_resolver.cancel();
        }
    });

    _resolver.async_resolve(endpoint.host, std::to_string(endpoint.port),
        [self, timer, handler](const asio::error_code& ec,
                               asio::ip::udp::resolver::results_type results) {
            timer->cancel();
            if (ec) {
                self->SetError(Core::Error::Code::ConnectionRefused, ec.message());
                LOG_WARN("Channel", "UDP DNS 解析失败: %s (asio=%d %s)",
                         self->_channelId.c_str(), ec.value(), ec.message().c_str());
                handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, ec.message()));
                return;
            }
            if (results.empty()) {
                self->SetError(Core::Error::Code::ConnectionRefused, "no udp endpoint");
                handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, "no endpoint"));
                return;
            }
            self->_peer = *results.begin();
            asio::error_code opc;
            if (!self->_socket.is_open()) {
                self->_socket.open(self->_peer.protocol(), opc);
            }
            if (opc) {
                self->SetError(Core::Error::Code::ConnectionRefused, opc.message());
                handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, opc.message()));
                return;
            }
            self->_connected.store(true);
            LOG_INFO("Channel", "UDP 就绪成功: %s", self->_channelId.c_str());
            handler(Core::Expected<void>());
        });
}

void UdpChannel::SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                             ReceiveHandler handler) {
    if (request.empty()) {
        LOG_ERROR("Channel", "SendReceive 拒绝空请求帧: channel=%s", _channelId.c_str());
        handler(Core::Unexpected(Core::Error::Code::BuildError,
                                 "empty request frame", "channel=" + _channelId));
        return;
    }
    if (!_connected.load() || !_socket.is_open()) {
        // 重连职责归 ChannelManager; 本方法不重连, 直接返回 ConnectionClosed。
        handler(Core::Unexpected(Core::Error::Code::ConnectionClosed, "udp not ready"));
        return;
    }
    // 单通道单请求守卫: 数据报无连接标识, 叠发会与迟到的应答混淆。
    if (_inFlight.exchange(true)) {
        handler(Core::Unexpected(Core::Error::Code::Busy,
                                 "channel busy: request in flight"));
        return;
    }
    ReceiveHandler wrapped = [this, handler](Core::Expected<Core::Bytes> r) {
        _inFlight.store(false);
        handler(std::move(r));
    };

    auto self = shared_from_this();
    auto op = std::make_shared<RxOp>(std::move(wrapped));
    op->maxFrameSize = framingConfig ? MaxFrameSizeOf(*framingConfig) : 1024;

    // 收发看门狗: 到期关闭 socket 判 Timeout (与 TCP 一致, 强制下轮重建以丢弃迟到数据报)
    op->watchdog = std::make_shared<asio::steady_timer>(_io);
    op->watchdog->expires_after(timeout);
    op->watchdog->async_wait([self, op](const asio::error_code& ec) {
        if (!ec && !op->done) {
            op->done = true;
            asio::error_code ignore;
            self->_socket.close(ignore);
            self->_connected.store(false);
            self->SetError(Core::Error::Code::Timeout, "udp send/receive timeout");
            op->handler(Core::Unexpected(Core::Error::Code::Timeout, "udp send/receive timeout"));
        }
    });

    // 请求字节由本操作持有到 send 完成 (asio 不拷贝数据)。
    auto frame = std::make_shared<Bytes>(request);
    _socket.async_send_to(asio::buffer(*frame), _peer,
        [self, op, frame](const asio::error_code& ec, size_t /*bytes*/) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                self->_connected.store(false);
                self->SetError(Core::Error::Code::WriteFailed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::WriteFailed, ec.message()));
                return;
            }
            self->ReceiveOne(op);
        });
}

void UdpChannel::ReceiveOne(const std::shared_ptr<RxOp>& op) {
    auto self = shared_from_this();
    auto buf = std::make_shared<Core::Bytes>(ReceiveCapOf(op->maxFrameSize));
    auto sender = std::make_shared<asio::ip::udp::endpoint>();
    _socket.async_receive_from(asio::buffer(*buf), *sender,
        [self, op, buf, sender](const asio::error_code& ec, size_t n) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                self->_connected.store(false);
                self->SetError(Core::Error::Code::ConnectionClosed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::ConnectionClosed, ec.message()));
                return;
            }
            // 一个数据报即完整响应: 数据报长度 = 响应长度, 无拼接。
            op->done = true;
            op->watchdog->cancel();
            buf->resize(n);
            op->handler(Core::Expected<Core::Bytes>(std::move(*buf)));
        });
}

void UdpChannel::Send(const Bytes& request,
                      std::chrono::milliseconds timeout,
                      SendHandler handler) {
    if (request.empty()) {
        LOG_ERROR("Channel", "Send 拒绝空请求帧: channel=%s", _channelId.c_str());
        handler(Core::Unexpected(Core::Error::Code::BuildError,
                                 "empty request frame", "channel=" + _channelId));
        return;
    }
    if (!_connected.load() || !_socket.is_open()) {
        handler(Core::Unexpected(Core::Error::Code::ConnectionClosed, "udp not ready"));
        return;
    }
    if (_inFlight.exchange(true)) {
        handler(Core::Unexpected(Core::Error::Code::Busy,
                                 "channel busy: request in flight"));
        return;
    }
    SendHandler wrapped = [this, handler](Core::Expected<void> r) {
        _inFlight.store(false);
        handler(std::move(r));
    };

    auto self = shared_from_this();
    auto op = std::make_shared<TxOp>(std::move(wrapped));
    auto frame = std::make_shared<Bytes>(request);

    op->watchdog = std::make_shared<asio::steady_timer>(_io);
    op->watchdog->expires_after(timeout);
    op->watchdog->async_wait([self, op](const asio::error_code& ec) {
        if (!ec && !op->done) {
            op->done = true;
            asio::error_code ignore;
            self->_socket.close(ignore);
            self->_connected.store(false);
            self->SetError(Core::Error::Code::Timeout, "udp send timeout");
            op->handler(Core::Unexpected(Core::Error::Code::Timeout, "udp send timeout"));
        }
    });

    // 单向发送: emit 数据报即完成, 不读回 (设备不应答, 如广播写)。
    _socket.async_send_to(asio::buffer(*frame), _peer,
        [self, op, frame](const asio::error_code& ec, size_t /*bytes*/) {
            if (op->done) return;
            op->done = true;
            op->watchdog->cancel();
            if (ec) {
                self->_connected.store(false);
                self->SetError(Core::Error::Code::WriteFailed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::WriteFailed, ec.message()));
                return;
            }
            op->handler(Core::Expected<void>());
        });
}

void UdpChannel::Disconnect() {
    bool wasConnected = _connected.exchange(false);
    if (!wasConnected && !_socket.is_open()) return;
    LOG_INFO("Channel", "UDP 关闭: %s (wasConnected=%d)",
             _channelId.c_str(), (int)wasConnected);
    asio::error_code ec;
    _socket.close(ec);   // 挂起操作以 operation_aborted 完成
}

bool UdpChannel::IsConnected() const {
    return _connected.load();
}

void UdpChannel::SetError(Core::Error::Code code, const std::string& msg) {
    std::lock_guard<std::mutex> lock(_errorMutex);
    _lastError.code = code;
    _lastError.message = msg;
    _lastError.context = _channelId;
}

Core::Error UdpChannel::GetLastError() const {
    std::lock_guard<std::mutex> lock(_errorMutex);
    return _lastError;
}

}} // namespace MyProt::Transport

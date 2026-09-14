// src/Transport/src/TcpChannel.cpp
// TCP 通道实现 — asio 异步回调模型 (C++11, ADR-0010 §2)
// 注意: 工程定义 ASIO_NO_DEPRECATED, 仅用新版 API (async_resolve(host,port) / asio::post)

#include "MyProt/Transport/TcpChannel.hpp"
#include "MyProt/Transport/SocketKeepAlive.hpp"
#include "MyProt/Core/Log.hpp"
#include <asio/connect.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <asio/error.hpp>
#include <utility>

namespace MyProt { namespace Transport {

namespace {

/// 判断收流缓冲是否已构成完整帧 (按成帧配置; LengthField 语义与 v1 对齐:
/// lengthIncludesHeader=true → total = len + adjustment
/// lengthIncludesHeader=false → total = headerLength + len + adjustment)
bool FrameComplete(const Core::Bytes& b, const Core::FramingConfig& f) {
    switch (f.type) {
    case Core::FramingType::Fixed:
        return b.size() >= static_cast<size_t>(f.fixed.fixedLength);

    case Core::FramingType::LengthField: {
        const Core::LengthFieldConfig& lf = f.lengthField;
        size_t need = static_cast<size_t>(lf.lengthFieldOffset + lf.lengthFieldLength);
        if (b.size() < need) return false;

        const uint8_t* p = b.data() + lf.lengthFieldOffset;
        unsigned long long lenVal = 0;
        bool bigEndian = (lf.byteOrder == Core::ByteOrder::BigEndian);
        switch (lf.lengthFieldLength) {
        case 1:
            lenVal = p[0];
            break;
        case 2:
            lenVal = bigEndian
                ? ((static_cast<unsigned long long>(p[0]) << 8) | p[1])
                : ((static_cast<unsigned long long>(p[1]) << 8) | p[0]);
            break;
        case 4:
            lenVal = bigEndian
                ? ((static_cast<unsigned long long>(p[0]) << 24) |
                   (static_cast<unsigned long long>(p[1]) << 16) |
                   (static_cast<unsigned long long>(p[2]) << 8)  | p[3])
                : ((static_cast<unsigned long long>(p[3]) << 24) |
                   (static_cast<unsigned long long>(p[2]) << 16) |
                   (static_cast<unsigned long long>(p[1]) << 8)  | p[0]);
            break;
        default:
            return true;   // 配置非法 → 交由上层解析阶段报错
        }

        size_t total = lf.lengthIncludesHeader
            ? static_cast<size_t>(lenVal) + static_cast<size_t>(lf.lengthAdjustment)
            : static_cast<size_t>(lf.headerLength) +
              static_cast<size_t>(lenVal) +
              static_cast<size_t>(lf.lengthAdjustment);
        return b.size() >= total;
    }

    case Core::FramingType::Silence:
        // TCP 面向字节流, 无字符间隙概念: 退化为"首个数据块即完整响应"
        // (请求-应答模式下设备一次性回包)
        return true;

    case Core::FramingType::Message:
        return true;   // CAN 预留 (ADR-0002), TCP 不应到达
    }
    return true;
}

/// 取当前成帧分支的帧长上限 (防恶意/异常超长帧)
size_t MaxFrameSizeOf(const Core::FramingConfig& f) {
    switch (f.type) {
    case Core::FramingType::Fixed:       return static_cast<size_t>(f.fixed.fixedLength);
    case Core::FramingType::Silence:     return static_cast<size_t>(f.silence.maxFrameSize);
    default:                             return static_cast<size_t>(f.lengthField.maxFrameSize);
    }
}

} // namespace

TcpChannel::TcpChannel(asio::io_context& io, const std::string& channelId)
    : _io(io)
    , _channelId(channelId)
    , _socket(new asio::ip::tcp::socket(io))
    , _resolver(io)
    , _connected(false)
    , _inFlight(false) {}

TcpChannel::~TcpChannel() {
    Disconnect();
}

void TcpChannel::Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) {
    if (_connected.load()) {
        // 已连接 → 直接成功 (共享总线复用语义)
        asio::post(_io, [handler]() { handler(Core::Expected<void>()); });
        return;
    }
    if (endpoint.host.empty()) {
        SetError(Core::Error::Code::ConnectionRefused, "tcp connect: empty host");
        asio::post(_io, [handler]() {
            handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, "empty host"));
        });
        return;
    }
        // port=0 的有效端口解析在 ChannelManager::PerformConnect (协议 defaultPort,
    // ADR-0012 附录A.1); 此处 502 仅为防御性兜底 (直用通道未经理由层的场景)。
    uint16_t port = endpoint.port != 0 ? endpoint.port : 502;

    LOG_INFO("Channel", "TCP 连接开始: %s:%u (timeout=%lldms, channel=%s)",
             endpoint.host.c_str(), (unsigned)port,
             (long long)timeout.count(), _channelId.c_str());

    auto self = shared_from_this();

    // 连接看门狗: 到期关闭套接字, 挂起的 resolve/connect 以 operation_aborted 结束
    auto timer = std::make_shared<asio::steady_timer>(_io);
    timer->expires_after(timeout);
    timer->async_wait([self](const asio::error_code& ec) {
        if (!ec && !self->_connected.load()) {
            asio::error_code ignore;
            self->_socket->close(ignore);
        }
    });

    auto onResolved = [self, timer, handler](const asio::error_code& ec,
                                             asio::ip::tcp::resolver::results_type results) {
        if (ec) {
            timer->cancel();
            self->SetError(Core::Error::Code::ConnectionRefused, ec.message());
            LOG_WARN("Channel", "TCP DNS 解析失败: %s (asio=%d %s)",
                     self->_channelId.c_str(), ec.value(), ec.message().c_str());
            handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, ec.message()));
            return;
        }
        asio::async_connect(*self->_socket, results,
            [self, timer, handler](const asio::error_code& ec2,
                                   const asio::ip::tcp::endpoint& /*endpoint*/) {
                timer->cancel();
                if (ec2 == asio::error::operation_aborted) {
                    LOG_WARN("Channel", "TCP 连接超时: %s", self->_channelId.c_str());
                    handler(Core::Unexpected(Core::Error::Code::Timeout, "connect timeout"));
                    return;
                }
                if (ec2) {
                    self->SetError(Core::Error::Code::ConnectionRefused, ec2.message());
                    LOG_WARN("Channel", "TCP 连接失败: %s (asio=%d %s)",
                             self->_channelId.c_str(), ec2.value(), ec2.message().c_str());
                    handler(Core::Unexpected(Core::Error::Code::ConnectionRefused, ec2.message()));
                    return;
                }
                // 关闭 Nagle: 小帧请求-应答场景降低延迟
                asio::error_code optEc;
                self->_socket->set_option(asio::ip::tcp::no_delay(true), optEc);
                // 启用 TCP keepalive: 解决"对端静默关闭后无法感知"问题。
                // 仿真器/PLC 关闭后, OS 2s 内检测到 RST, 下次 SendReceive 立即失败,
                // Session 断路器累加 → 下次轮询周期自然触发重连 (无需新 API)。
                // 参数: idle=2s 空闲后开始探, intvl=2s 探一次, 连续 3 次失败判死 (≈6s 总耗时)。
                SetSocketKeepAlive(self->_socket->native_handle(),
                                   true, 2, 2, 3);
                self->_connected.store(true);
                LOG_INFO("Channel", "TCP 连接成功: %s", self->_channelId.c_str());
                handler(Core::Expected<void>());
            });
    };

    // ASIO_NO_DEPRECATED: 使用字符串对重载 (无 resolver::query)
    _resolver.async_resolve(endpoint.host, std::to_string(port), onResolved);
}

void TcpChannel::SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                              ReceiveHandler handler) {
    if (request.empty()) {
        // 空请求帧拦截: 直接快速失败, 避免 asio::buffer(空 vector) 触发 debug 断言崩溃
        LOG_ERROR("Channel", "SendReceive 拒绝空请求帧: channel=%s", _channelId.c_str());
        handler(Core::Unexpected(Core::Error::Code::BuildError,
                                 "empty request frame", "channel=" + _channelId));
        return;
    }
    if (!_connected.load() || !_socket->is_open()) {
        // 未连接或 socket 已关闭: 不在本方法内重连, 直接返回 ConnectionClosed。
        // 理由: 重连职责归属 ChannelManager (入口已有 2s 冷却期节流),
        //       递归重发业务请求会与 ChannelManager 端的 GOC 形成双重 Connect 风暴。
        //       PollingEngine 收到 ConnectionClosed 后会按 IsRetryable → 退避重试,
        //       下一轮 GetOrCreateChannel 自然走完整重连链。
        handler(Core::Unexpected(Core::Error::Code::ConnectionClosed, "tcp not connected"));
        return;
    }

    // 单通道单请求守卫: 占位失败 = 已有在途请求 → Busy 快速失败。
    // 清位时机: 包裹后的 handler 首次被调 (超时/错误/成帧/断开所有终态
    // 均经 op->handler 恰好一次), 保证不泄漏占位。
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
    op->framing = framingConfig;
    if (!op->framing) {
        op->framing = std::make_shared<const Core::FramingConfig>();  // 默认 LengthField
    }
    op->maxFrameSize = MaxFrameSizeOf(*op->framing);

    // 收流看门狗: 到期判 Timeout 并关闭套接字
    op->watchdog = std::make_shared<asio::steady_timer>(_io);
    op->watchdog->expires_after(timeout);
    op->watchdog->async_wait([self, op](const asio::error_code& ec) {
        if (!ec && !op->done) {
            op->done = true;
            asio::error_code ignore;
            self->_socket->close(ignore);
            self->_connected.store(false);   // 标记断连, 下次轮询触发重连
            self->SetError(Core::Error::Code::Timeout, "send/receive timeout");
            op->handler(Core::Unexpected(Core::Error::Code::Timeout, "send/receive timeout"));
        }
    });

    asio::async_write(*_socket, asio::buffer(request),
        [self, op](const asio::error_code& ec, size_t /*bytes*/) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                self->_connected.store(false);   // 标记断连, 下次轮询触发重连
                self->SetError(Core::Error::Code::WriteFailed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::WriteFailed, ec.message()));
                return;
            }
            self->ReadChunk(op);
        });
}

void TcpChannel::ReadChunk(const std::shared_ptr<RxOp>& op) {
    auto self = shared_from_this();
    auto chunk = std::make_shared<std::vector<uint8_t>>(4096);
    _socket->async_read_some(asio::buffer(*chunk),
        [self, op, chunk](const asio::error_code& ec, size_t n) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                self->_connected.store(false);   // 标记断连, 下次轮询触发重连
                self->SetError(Core::Error::Code::ConnectionClosed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::ConnectionClosed, ec.message()));
                return;
            }
            op->buffer.insert(op->buffer.end(), chunk->begin(), chunk->begin() + n);

            if (op->buffer.size() > op->maxFrameSize) {
                op->done = true;
                op->watchdog->cancel();
                self->SetError(Core::Error::Code::ParseError, "frame exceeds maxFrameSize");
                op->handler(Core::Unexpected(Core::Error::Code::ParseError, "frame exceeds maxFrameSize"));
                return;
            }
            if (FrameComplete(op->buffer, *op->framing)) {
                op->done = true;
                op->watchdog->cancel();
                op->handler(Core::Expected<Core::Bytes>(std::move(op->buffer)));
                return;
            }
            self->ReadChunk(op);   // 未成帧 → 继续读
        });
}

void TcpChannel::Disconnect() {
    bool wasConnected = _connected.exchange(false);
    if (!wasConnected && (!_socket || !_socket->is_open())) return;
    LOG_INFO("Channel", "TCP 断开: %s (wasConnected=%d)",
             _channelId.c_str(), (int)wasConnected);
    asio::error_code ec;
    _socket->close(ec);   // 挂起操作以 operation_aborted 完成
}

bool TcpChannel::IsConnected() const {
    return _connected.load();
}

void TcpChannel::SetError(Core::Error::Code code, const std::string& msg) {
    std::lock_guard<std::mutex> lock(_errorMutex);
    _lastError.code = code;
    _lastError.message = msg;
    _lastError.context = _channelId;
}

Core::Error TcpChannel::GetLastError() const {
    std::lock_guard<std::mutex> lock(_errorMutex);
    return _lastError;
}

}} // namespace MyProt::Transport

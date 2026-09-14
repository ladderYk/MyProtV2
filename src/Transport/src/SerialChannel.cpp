// src/Transport/src/SerialChannel.cpp
// 串口通道实现 — 静默成帧读循环 (C++11, ADR-0010 §2)
//
// 收流模型: 发送请求 → async_read_some 累积字节, 每次到达重启静默定时器;
// 定时器到期 (字符间隔超过 frameGap) 即判定一帧完成 → 回调 handler。
// 看门狗超时 → Timeout 完成。Disconnect/错误 → 关闭端口中止挂起操作。

#include "MyProt/Transport/SerialChannel.hpp"
#include <asio/write.hpp>
#include <asio/error.hpp>
#include <utility>

namespace MyProt { namespace Transport {

namespace {

/// 计算帧间静默阈值 — 不默认任何折算 (引擎不内置 Modbus RTU 的 3.5×单字符时间约定).
/// 引擎零协议知识: frameGapUs 必须显式配置, 未配置由 SendReceive 前置检查报错.
std::chrono::microseconds SilenceGapUs(const Core::SilenceConfig& sc) {
    return std::chrono::microseconds(sc.frameGapUs);
}

} // namespace

struct SerialChannel::OpState {
    explicit OpState(ReceiveHandler h)
        : handler(std::move(h))
        , scratch(256)
        , maxFrameSize(256)
        , gapUs(std::chrono::microseconds(1750))
        , done(false) {}

    ReceiveHandler handler;
    Core::Bytes buffer;                       // 已累积的响应字节
    std::vector<uint8_t> scratch;             // 单次 read_some 暂存
    std::shared_ptr<asio::steady_timer> gapTimer;     // 帧间静默定时器
    std::shared_ptr<asio::steady_timer> watchdog;     // 整体超时看门狗
    size_t maxFrameSize;
    std::chrono::microseconds gapUs;
    bool done;
};

SerialChannel::SerialChannel(asio::io_context& io,
                             const Core::SerialTransportConfig& serialConfig,
                             const std::string& channelId)
    : _io(io)
    , _serial(serialConfig)
    , _channelId(channelId)
    , _connected(false) {}

void SerialChannel::Connect(const Core::ConnectionConfig& endpoint,
                            std::chrono::milliseconds timeout,
                            ConnectHandler handler) {
    // 本地串口打开为同步快操作; timeout 参数保留接口一致性
    (void)timeout;

    std::string portName = !endpoint.portName.empty()
                         ? endpoint.portName : _serial.portName;
    if (portName.empty()) {
        SetError(Core::Error::Code::ConfigError, "serial connect: portName empty");
        handler(Core::Unexpected(Core::Error::Code::ConfigError,
                                 "serial: portName not configured"));
        return;
    }

    auto port = std::make_shared<asio::serial_port>(_io);
    asio::error_code ec;
    port->open(portName, ec);
    if (ec) {
        SetError(Core::Error::Code::ConnectionRefused, ec.message());
        handler(Core::Unexpected(Core::Error::Code::ConnectionRefused,
                                 "open " + portName + ": " + ec.message()));
        return;
    }

    using sb = asio::serial_port_base;

    port->set_option(sb::baud_rate(_serial.baudRate), ec);
    if (!ec) port->set_option(sb::character_size(_serial.dataBits), ec);

    if (!ec) {
        sb::parity::type pt = sb::parity::none;
        if (_serial.parity == Core::SerialTransportConfig::Parity::Odd) pt = sb::parity::odd;
        else if (_serial.parity == Core::SerialTransportConfig::Parity::Even) pt = sb::parity::even;
        port->set_option(sb::parity(pt), ec);
    }
    if (!ec) {
        sb::stop_bits::type st = (_serial.stopBits == Core::SerialTransportConfig::StopBits::Two)
                               ? sb::stop_bits::two : sb::stop_bits::one;
        port->set_option(sb::stop_bits(st), ec);
    }
    if (!ec) port->set_option(sb::flow_control(sb::flow_control::none), ec);

    if (ec) {
        asio::error_code ignore;
        port->close(ignore);
        SetError(Core::Error::Code::ConnectionRefused, ec.message());
        handler(Core::Unexpected(Core::Error::Code::ConnectionRefused,
                                 "set_option: " + ec.message()));
        return;
    }

    _port = port;
    _connected.store(true);
    handler(Core::Expected<void>());
}

void SerialChannel::SendReceive(const Bytes& request,
                                std::shared_ptr<const Core::FramingConfig> framingConfig,
                                std::chrono::milliseconds timeout,
                                ReceiveHandler handler) {
    auto port = _port;
    if (!_connected.load() || !port) {
        handler(Core::Unexpected(Core::Error::Code::ConnectionClosed,
                                 "serial not connected"));
        return;
    }

    // 成帧参数: 仅接受 Silence 分支 (串口语义)
    Core::SilenceConfig sc;
    if (framingConfig && framingConfig->type == Core::FramingType::Silence) {
        sc = framingConfig->silence;
    }
        // 静默阈值须显式配置 — 引擎不默认 3.5×单字符时间 (Modbus RTU 约定).
    //   未配置 Silence 成帧或 frameGapUs<=0 时直接报 ConfigError, 避免通用串口
    //   通道静默继承 RTU 行为 (协议知识泄漏进引擎).
    if (sc.frameGapUs <= 0) {
        handler(Core::Unexpected(Core::Error::Code::ConfigError,
            "serial: 静默成帧须显式配置 framing.silence.frameGapUs (>0); "
            "引擎已移除 3.5×charTimeUs 默认 (Modbus RTU 约定), 请按协议填写帧间静默阈值"));
        return;
    }

    auto self = shared_from_this();
    auto op = std::make_shared<OpState>(std::move(handler));
    op->gapUs = SilenceGapUs(sc);
    op->maxFrameSize = sc.maxFrameSize > 0
                     ? static_cast<size_t>(sc.maxFrameSize) : static_cast<size_t>(256);
    op->gapTimer = std::make_shared<asio::steady_timer>(_io);
    op->watchdog = std::make_shared<asio::steady_timer>(_io);

    // 看门狗: 到期判 Timeout 并关闭端口中止收流
    op->watchdog->expires_after(timeout);
    op->watchdog->async_wait([self, op, port](const asio::error_code& ec) {
        if (!ec && !op->done) {
            op->done = true;
            asio::error_code ignore;
            port->close(ignore);
            // 连接级故障: 必须复位连接标志, 否则 IsConnected() 恒 true →
            // GetOrCreateChannel 快路径复用已关闭端口 → 该设备永久失效。
            self->_connected.store(false);
            self->_port.reset();
            self->SetError(Core::Error::Code::Timeout, "serial receive timeout");
            op->handler(Core::Unexpected(Core::Error::Code::Timeout,
                                         "serial receive timeout"));
        }
    });

    // 发送请求 → 启动静默窗口 + 读循环
    asio::async_write(*port, asio::buffer(request),
        [self, op, port](const asio::error_code& ec, size_t /*bytes*/) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                self->SetError(Core::Error::Code::WriteFailed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::WriteFailed,
                                             ec.message()));
                return;
            }

            // 初始静默窗口: 写入后 gap 内无任何字节到达 → 设备无响应 (Timeout)
            op->gapTimer->expires_after(op->gapUs);
            op->gapTimer->async_wait([self, op, port](const asio::error_code& gec) {
                if (!gec && !op->done) {
                    op->done = true;
                    op->watchdog->cancel();
                    if (op->buffer.empty()) {
                        self->SetError(Core::Error::Code::Timeout,
                                       "serial: no response");
                        op->handler(Core::Unexpected(Core::Error::Code::Timeout,
                                                     "serial: no response"));
                    } else {
                        op->handler(Core::Expected<Core::Bytes>(std::move(op->buffer)));
                    }
                }
            });

            self->ReadSome(op, port);
        });
}

void SerialChannel::ReadSome(const std::shared_ptr<OpState>& op,
                             const std::shared_ptr<asio::serial_port>& port) {
    auto self = shared_from_this();
    port->async_read_some(asio::buffer(op->scratch),
        [self, op, port](const asio::error_code& ec, size_t n) {
            if (op->done) return;
            if (ec) {
                op->done = true;
                op->watchdog->cancel();
                // 同上: 读错误属连接级故障, 复位连接标志以便下次重连
                self->_connected.store(false);
                self->_port.reset();
                self->SetError(Core::Error::Code::ConnectionClosed, ec.message());
                op->handler(Core::Unexpected(Core::Error::Code::ConnectionClosed,
                                             ec.message()));
                return;
            }

            op->buffer.insert(op->buffer.end(),
                              op->scratch.begin(), op->scratch.begin() + n);

            if (op->buffer.size() > op->maxFrameSize) {
                op->done = true;
                op->watchdog->cancel();
                self->SetError(Core::Error::Code::ParseError,
                               "serial frame exceeds maxFrameSize");
                op->handler(Core::Unexpected(Core::Error::Code::ParseError,
                                             "frame exceeds maxFrameSize"));
                return;
            }

            // 字符到达 → 重启静默定时器后继续读 (到期即成帧完成)
            op->gapTimer->cancel();
            op->gapTimer->expires_after(op->gapUs);
            op->gapTimer->async_wait([self, op, port](const asio::error_code& gec) {
                if (!gec && !op->done) {
                    op->done = true;
                    op->watchdog->cancel();
                    op->handler(Core::Expected<Core::Bytes>(std::move(op->buffer)));
                }
            });

            self->ReadSome(op, port);
        });
}

void SerialChannel::Disconnect() {
    _connected.store(false);
    auto port = _port;
    _port.reset();
    if (port) {
        asio::error_code ec;
        port->close(ec);   // 中止挂起的 read/write/timer 关联回调
    }
}

bool SerialChannel::IsConnected() const {
    return _connected.load();
}

void SerialChannel::SetError(Core::Error::Code code, const std::string& msg) {
    std::lock_guard<std::mutex> lock(_errorMutex);
    _lastError.code = code;
    _lastError.message = msg;
    _lastError.context = _channelId;
}

Core::Error SerialChannel::GetLastError() const {
    std::lock_guard<std::mutex> lock(_errorMutex);
    return _lastError;
}

}} // namespace MyProt::Transport

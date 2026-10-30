// src/Northbound/src/MqttClient.cpp
// 北向 MQTT 3.1.1 发布客户端实现
// 状态机: BeginConnect → resolve → tcp connect → CONNECT/CONNACK → connected
//         任一失败 → 关闭 socket → 指数退避重连 (1s → 2s → ... → 30s)
// 发送串行化: 所有 PUBLISH/PINGREQ 进入 _txQueue, 同一时刻只有一个 async_write

#include "MyProt/Northbound/MqttClient.hpp"

#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <chrono>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Northbound/MqttCodec.hpp"

namespace MyProt { namespace Northbound {

namespace {

const int kConnectTimeoutSec = 10;
const int kInitialReconnectMs = 1000;   // fallback when the config leaves the field unset
const int kMaxReconnectMs = 30000;
const std::size_t kMaxTxQueue = 4096;
const std::size_t kReadChunk = 4096;

int InitialReconnectMs(const Core::MqttConfig& cfg) {
    return cfg.reconnectMinMs > 0 ? cfg.reconnectMinMs : kInitialReconnectMs;
}

int MaxReconnectMs(const Core::MqttConfig& cfg) {
    return (cfg.reconnectMaxMs > 0 && cfg.reconnectMaxMs >= InitialReconnectMs(cfg))
        ? cfg.reconnectMaxMs : kMaxReconnectMs;
}

} // namespace

MqttClient::MqttClient(asio::io_context& io, const Core::MqttConfig& cfg)
    : _io(io)
    , _cfg(cfg)
    , _codec(new MqttCodec())
    , _session(*_codec, cfg)
    , _socket()
    , _resolver(io)
    , _reconnectTimer(new asio::steady_timer(io))
    , _connectTimer(new asio::steady_timer(io))
    , _keepAliveTimer(new asio::steady_timer(io))
    , _started(false)
    , _stopping(true)
    , _connected(false)
    , _reading(false)
    , _writing(false)
    , _attemptInFlight(false)
    , _reconnectScheduled(false)
    , _pingOutstanding(false)
    , _reconnectDelayMs(InitialReconnectMs(cfg))
    , _droppedPackets(0) {
    // QoS1 PUBACK 由会话产生, 经发送队列落地; 此时尚未 Connected, 但只有已连接时才会回调
    _session.SetSendHandler([this](const std::vector<uint8_t>& packet) {
        EnqueuePacket(packet);
    });
}

MqttClient::~MqttClient() {
    _stopping.store(true);
    CloseSocket();
}

void MqttClient::SetWill(const std::string& willTopic,
                         const std::string& willPayload,
                         int willQos,
                         bool willRetain) {
    _session.SetWill(willTopic, willPayload, willQos, willRetain);
}

void MqttClient::SetOnConnected(ConnectedHandler handler) {
    _onConnected = handler;
}

void MqttClient::Start() {
    bool expected = false;
    if (!_started.compare_exchange_strong(expected, true)) return;
    _stopping.store(false);
    auto self = shared_from_this();
    asio::post(_io, [self]() { self->BeginConnect(); });
}

void MqttClient::Stop() {
    if (!_started.load()) return;
    _stopping.store(true);
    _connected.store(false);
    auto self = shared_from_this();
    asio::post(_io, [self]() {
        asio::error_code ec;
        self->_reconnectTimer->cancel(ec);
        self->_connectTimer->cancel(ec);
        self->_keepAliveTimer->cancel(ec);
        self->CloseSocket();
        self->_txQueue.clear();
        self->_rxBuf.clear();
    });
}

bool MqttClient::Publish(const std::string& topic,
                         const std::string& payload,
                         int qos,
                         bool retain) {
    if (_stopping.load() || !_connected.load()) return false;

    auto self = shared_from_this();
    asio::post(_io, [self, topic, payload, qos, retain]() {
        if (self->_stopping.load() || !self->_connected.load()) return;
        std::vector<uint8_t> packet;
        if (!self->_session.BuildPublish(topic, payload, qos, retain, packet)) return;
        self->EnqueuePacket(packet);
    });
    return true;
}

std::string MqttClient::LastError() const {
    std::lock_guard<std::mutex> lock(_errorMutex);
    return _lastError;
}

// ──────────────────── 连接状态机 ────────────────────

void MqttClient::BeginConnect() {
    if (_stopping.load()) return;
    if (_attemptInFlight) return;
    _attemptInFlight = true;
    _reading = false;
    _writing = false;
    _pingOutstanding = false;
    _rxBuf.clear();
    _txQueue.clear();
    _connected.store(false);

    CloseSocket();
    _socket.reset(new asio::ip::tcp::socket(_io));

    // 总看门狗: 覆盖 DNS + TCP + CONNACK, 到期关闭 socket 触发重连
    auto self = shared_from_this();
    _connectTimer->expires_after(std::chrono::seconds(kConnectTimeoutSec));
    _connectTimer->async_wait([self](const asio::error_code& ec) {
        if (ec || self->_stopping.load()) return;
        if (!self->_attemptInFlight) return;
        self->FailAndReconnect("connect/connack timeout");
    });

    LOG_DEBUG("MQTT", "连接 broker %s:%d ...", _cfg.host.c_str(), _cfg.port);
    _resolver.async_resolve(
        _cfg.host, std::to_string(_cfg.port),
        [self](const asio::error_code& ec,
               asio::ip::tcp::resolver::results_type results) {
            self->OnResolved(ec, results);
        });
}

void MqttClient::OnResolved(const asio::error_code& ec,
                            asio::ip::tcp::resolver::results_type results) {
    if (_stopping.load() || !_attemptInFlight) return;
    if (ec) {
        FailAndReconnect("resolve: " + ec.message());
        return;
    }
    auto self = shared_from_this();
    asio::async_connect(*_socket, results,
        [self](const asio::error_code& ec2,
               const asio::ip::tcp::endpoint& /*ep*/) {
            self->OnTcpConnected(ec2);
        });
}

void MqttClient::OnTcpConnected(const asio::error_code& ec) {
    if (_stopping.load() || !_attemptInFlight) return;
    if (ec) {
        FailAndReconnect("tcp connect: " + ec.message());
        return;
    }
    asio::error_code optEc;
    _socket->set_option(asio::ip::tcp::no_delay(true), optEc);
    SendConnectPacket();
}

void MqttClient::SendConnectPacket() {
    std::vector<uint8_t> packet;
    _session.BuildConnect(packet);

    auto self = shared_from_this();
    auto frame = std::make_shared<std::vector<uint8_t> >(packet);
    asio::async_write(*_socket, asio::buffer(*frame),
        [self, frame](const asio::error_code& ec, std::size_t /*n*/) {
            if (self->_stopping.load()) return;
            if (ec) {
                self->FailAndReconnect("send CONNECT: " + ec.message());
                return;
            }
            // CONNECT 之后立即开始读, 等待 CONNACK
            self->StartRead();
        });
}

void MqttClient::StartRead() {
    if (_reading || _stopping.load() || !_socket || !_socket->is_open()) return;
    _reading = true;
    ReadSome();
}

void MqttClient::ReadSome() {
    auto self = shared_from_this();
    auto chunk = std::make_shared<std::vector<uint8_t> >(kReadChunk);
    _socket->async_read_some(asio::buffer(*chunk),
        [self, chunk](const asio::error_code& ec, std::size_t n) {
            self->_reading = false;
            if (self->_stopping.load()) return;
            if (ec) {
                if (ec == asio::error::operation_aborted) return;
                self->FailAndReconnect("read: " + ec.message());
                return;
            }
            self->_rxBuf.insert(self->_rxBuf.end(),
                                chunk->begin(),
                                chunk->begin() + static_cast<std::ptrdiff_t>(n));
            if (!self->ProcessRx()) return;
            self->StartRead();
        });
}

bool MqttClient::ProcessRx() {
    std::size_t offset = 0;
    while (offset < _rxBuf.size()) {
        Core::Frame frame;
        std::size_t consumed = 0;
        const Core::DecodeStatus status = _codec->Decode(
            Core::ByteView(&_rxBuf[0] + offset, _rxBuf.size() - offset),
            consumed, frame);
        if (status == Core::DecodeStatus::NeedMore) break;
        if (status == Core::DecodeStatus::Malformed) {
            FailAndReconnect("malformed packet");
            return false;
        }
        if (consumed == 0 || consumed > _rxBuf.size() - offset) {
            FailAndReconnect("codec consumed out of range");
            return false;
        }
        offset += consumed;
        // 收到任意报文即说明链路存活 (PINGRESP 尤其如此)
        _pingOutstanding = false;
        if (!HandleFrame(frame)) return false;
    }
    if (offset > 0) {
        _rxBuf.erase(_rxBuf.begin(),
                     _rxBuf.begin() + static_cast<std::ptrdiff_t>(offset));
    }
    return true;
}

bool MqttClient::HandleFrame(const Core::Frame& frame) {
    const MqttSession::Event event = _session.OnFrame(frame);
    if (event == MqttSession::Event::Fatal) {
        FailAndReconnect(_session.LastError());
        return false;
    }
    if (event == MqttSession::Event::Connected) {
        OnEstablished();
    }
    return true;
}

void MqttClient::OnEstablished() {
    asio::error_code ec;
    _connectTimer->cancel(ec);
    _attemptInFlight = false;
    _reconnectScheduled = false;
    _reconnectDelayMs = InitialReconnectMs(_cfg);
    _connected.store(true);
    SetLastError(std::string());

    LOG_INFO("MQTT", "已连接 broker %s:%d (clientId=%s)",
             _cfg.host.c_str(), _cfg.port, _cfg.clientId.c_str());

    ArmKeepAlive();

    // 重连后恢复订阅 (cleanSession=true 时 broker 不会保留)
    const std::vector<std::vector<uint8_t> > subscriptions =
        _session.BuildSubscriptionPackets();
    for (std::size_t i = 0; i < subscriptions.size(); ++i) {
        EnqueuePacket(subscriptions[i]);
    }

    if (_onConnected) _onConnected();   // 重连后清空已发布缓存 → 全量补发
}

// ──────────────────── 订阅 ────────────────────

void MqttClient::Subscribe(const std::string& topic, int qos) {
    auto self = shared_from_this();
    asio::post(_io, [self, topic, qos]() {
        self->_session.AddSubscription(topic, qos);
        if (!self->_connected.load()) return;
        std::vector<uint8_t> packet;
        self->_session.BuildSubscribe(topic, qos, packet);
        self->EnqueuePacket(packet);
    });
}

void MqttClient::SetOnMessage(MessageHandler handler) {
    _session.SetOnMessage(handler);
}

void MqttClient::SetCleanSession(bool clean) {
    _session.SetCleanSession(clean);
}

// ──────────────────── 发送队列 ────────────────────

void MqttClient::EnqueuePacket(const std::vector<uint8_t>& packet) {
    if (_stopping.load() || !_connected.load()) return;
    if (_txQueue.size() >= kMaxTxQueue) {
        // 背压保护: broker 不读时不能无限堆内存 (遥测丢帧优于 OOM)
        ++_droppedPackets;
        if (_droppedPackets == 1 || (_droppedPackets % 1000) == 0) {
            LOG_WARN("MQTT", "发送队列已满 (%llu 帧被丢弃)",
                     static_cast<unsigned long long>(_droppedPackets));
        }
        return;
    }
    _txQueue.push_back(packet);
    StartWrite();
}

void MqttClient::StartWrite() {
    if (_writing || _txQueue.empty() || _stopping.load() || !_socket
            || !_socket->is_open()) {
        return;
    }
    _writing = true;
    auto self = shared_from_this();
    auto frame = std::make_shared<std::vector<uint8_t> >(_txQueue.front());
    _txQueue.pop_front();
    asio::async_write(*_socket, asio::buffer(*frame),
        [self, frame](const asio::error_code& ec, std::size_t /*n*/) {
            self->_writing = false;
            if (self->_stopping.load()) return;
            if (ec) {
                self->FailAndReconnect("write: " + ec.message());
                return;
            }
            self->StartWrite();
        });
}

// ──────────────────── 心跳 / 重连 ────────────────────

void MqttClient::ArmKeepAlive() {
    if (_cfg.keepAliveSec <= 0) return;   // 0 = 关闭心跳
    auto self = shared_from_this();
    _keepAliveTimer->expires_after(std::chrono::seconds(_cfg.keepAliveSec));
    _keepAliveTimer->async_wait([self](const asio::error_code& ec) {
        if (ec || self->_stopping.load() || !self->_connected.load()) return;
        if (self->_pingOutstanding) {
            self->FailAndReconnect("keepalive timeout (no PINGRESP)");
            return;
        }
        self->_pingOutstanding = true;
        std::vector<uint8_t> ping;
        self->_session.BuildPingreq(ping);
        self->EnqueuePacket(ping);
        self->ArmKeepAlive();
    });
}

void MqttClient::FailAndReconnect(const std::string& reason) {
    if (_stopping.load()) return;
    const bool wasConnected = _connected.exchange(false);
    _attemptInFlight = false;
    _reading = false;
    _writing = false;
    _pingOutstanding = false;

    CloseSocket();
    asio::error_code ec;
    _connectTimer->cancel(ec);
    _keepAliveTimer->cancel(ec);
    _txQueue.clear();
    _rxBuf.clear();
    SetLastError(reason);

    if (wasConnected) {
        LOG_WARN("MQTT", "连接断开: %s", reason.c_str());
    } else {
        LOG_DEBUG("MQTT", "连接失败: %s", reason.c_str());
    }
    ScheduleReconnect();
}

void MqttClient::ScheduleReconnect() {
    if (_stopping.load() || _reconnectScheduled) return;
    _reconnectScheduled = true;

    const int delayMs = _reconnectDelayMs;
    _reconnectDelayMs = std::min(_reconnectDelayMs * 2, MaxReconnectMs(_cfg));
    LOG_INFO("MQTT", "%dms 后重连 %s:%d", delayMs, _cfg.host.c_str(), _cfg.port);

    auto self = shared_from_this();
    _reconnectTimer->expires_after(std::chrono::milliseconds(delayMs));
    _reconnectTimer->async_wait([self](const asio::error_code& ec) {
        if (ec || self->_stopping.load()) return;
        self->_reconnectScheduled = false;
        self->BeginConnect();
    });
}

void MqttClient::CloseSocket() {
    if (!_socket) return;
    asio::error_code ec;
    if (_socket->is_open()) _socket->close(ec);
}

void MqttClient::SetLastError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(_errorMutex);
    _lastError = msg;
}

}} // namespace MyProt::Northbound

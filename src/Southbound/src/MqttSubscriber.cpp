// src/Southbound/src/MqttSubscriber.cpp

#include "MyProt/Southbound/MqttSubscriber.hpp"

#include "MyProt/Core/Log.hpp"
#include "MyProt/Core/MqttConfig.hpp"
#include "MyProt/Northbound/MqttClient.hpp"
#include "MyProt/Southbound/TagValueSink.hpp"

namespace MyProt { namespace Southbound {

MqttSubscriber::MqttSubscriber(asio::io_context& io,
                               const std::string& deviceId,
                               const Core::MqttIngestConfig& cfg,
                               const std::string& fallbackHost)
    : _io(io)
    , _deviceId(deviceId)
    , _cfg(cfg)
    , _fallbackHost(fallbackHost)
    , _running(false) {}

void MqttSubscriber::Start(FrameHandler onFrame, ErrorHandler onError) {
    if (_running.load()) return;
    _onFrame = onFrame;
    _onError = onError;

    Core::MqttConfig mc;
    mc.enabled = true;
    mc.host = _cfg.host.empty() ? _fallbackHost : _cfg.host;
    mc.port = _cfg.port == 0 ? 1883 : _cfg.port;
    mc.clientId = _cfg.clientId.empty()
        ? ("myprot-sub-" + _deviceId) : _cfg.clientId;
    if (_cfg.username.has_value()) mc.username = _cfg.username.value();
    if (_cfg.password.has_value()) mc.password = _cfg.password.value();
    mc.keepAliveSec = _cfg.keepAliveSec > 0 ? _cfg.keepAliveSec : 60;
    mc.reconnectMinMs = _cfg.reconnectMinMs;
    mc.reconnectMaxMs = _cfg.reconnectMaxMs;

    if (mc.host.empty()) {
        if (_onError) _onError("ingest.mqtt.host 为空且 device.connection.host 未设置");
        return;
    }

    _client = std::make_shared<Northbound::MqttClient>(_io, mc);
    _client->SetCleanSession(_cfg.cleanSession);
    _client->SetOnMessage([this](const std::string& topic, const std::string& payload) {
        if (!_onFrame) return;
        InboundFrame f;
        f.topic = topic;
        f.payload = payload;
        f.timestampMs = TagValueSink::NowMs();
        _onFrame(f);
    });
    _client->Subscribe(_cfg.topic, _cfg.qos);
    _client->Start();
    _running.store(true);

    LOG_INFO("Southbound", "MQTT 订阅已启动 device=%s broker=%s:%d topic=%s qos=%d",
             _deviceId.c_str(), mc.host.c_str(), static_cast<int>(mc.port),
             _cfg.topic.c_str(), _cfg.qos);
}

void MqttSubscriber::Stop() {
    _running.store(false);
    if (_client) {
        _client->Stop();
        _client.reset();
    }
    _onFrame = FrameHandler();
    _onError = ErrorHandler();
}

}} // namespace MyProt::Southbound

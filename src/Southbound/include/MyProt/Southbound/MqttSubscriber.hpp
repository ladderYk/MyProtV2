// src/Southbound/include/MyProt/Southbound/MqttSubscriber.hpp
// MQTT subscribe adapter - reuses the Northbound MqttClient/MqttCodec stack.

#pragma once
#include <asio.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Southbound/IInboundTransport.hpp"

namespace MyProt { namespace Northbound { class MqttClient; } }

namespace MyProt { namespace Southbound {

class MqttSubscriber : public IInboundTransport {
public:
    MqttSubscriber(asio::io_context& io,
                   const std::string& deviceId,
                   const Core::MqttIngestConfig& cfg,
                   const std::string& fallbackHost);

    void Start(FrameHandler onFrame, ErrorHandler onError);
    void Stop();
    bool IsRunning() const { return _running.load(); }

private:
    asio::io_context& _io;
    std::string _deviceId;
    Core::MqttIngestConfig _cfg;
    std::string _fallbackHost;

    std::shared_ptr<Northbound::MqttClient> _client;
    FrameHandler _onFrame;
    ErrorHandler _onError;
    std::atomic<bool> _running;

    MqttSubscriber(const MqttSubscriber&) = delete;
    MqttSubscriber& operator=(const MqttSubscriber&) = delete;
};

}} // namespace MyProt::Southbound

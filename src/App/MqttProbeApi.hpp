// src/App/MqttProbeApi.hpp - management-plane adapter for MQTT observation sessions
// HTTP semantics only: parse the request, call Northbound::MqttProbeHub, map errors to status codes.
// The hub itself (socket / session state) lives in Northbound and is injected by main.cpp.
#ifndef MYPROT_APP_MQTTPROBEAPI_HPP
#define MYPROT_APP_MQTTPROBEAPI_HPP

#include <string>
#include <utility>

#include "AppContext.hpp"

namespace MyProt { namespace Northbound { class MqttProbeHub; } }

namespace MyProt { namespace App {

/// /api/mqtt/probe[/{sid}[/publish]]
///   POST   /api/mqtt/probe                 start an observation session
///   GET    /api/mqtt/probe/{sid}           session status
///   DELETE /api/mqtt/probe/{sid}           stop the session
///   POST   /api/mqtt/probe/{sid}/publish   publish through the session's connection
std::pair<int, std::string> HandleMqttProbeApi(
    Northbound::MqttProbeHub& hub, const HttpRequest& req);

/// SSE provider for GET /api/mqtt/stream?sid=... — drains the session event queue.
/// An unknown/expired sid yields a "closed" frame; an empty string skips the round.
std::string BuildMqttProbeStream(
    Northbound::MqttProbeHub& hub, const std::string& path);

}} // namespace MyProt::App

#endif // MYPROT_APP_MQTTPROBEAPI_HPP

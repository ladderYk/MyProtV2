// src/Core/include/MyProt/Core/MqttConfig.hpp
// Northbound MQTT publisher configuration (server.json "mqtt" block).
//
// Scope (v1): outbound publication only - the gateway maps the latest-value store
// to MQTT topics. Inbound command subscription is a separate stage and is not
// represented here yet.
//
// File: configs/server.json, loaded by ConfigDirectoryLoader; optional block,
// absent = disabled. The block is validated only when enabled=true.
#pragma once

#include <string>

namespace MyProt {
namespace Core {

struct MqttConfig {
    bool enabled;                 // false = no MQTT client is created at all
    std::string host;             // broker host (no scheme)
    int port;                     // broker TCP port
    std::string clientId;         // MQTT client identifier (must be unique per broker)
    std::string username;         // optional; empty = anonymous
    std::string password;         // optional
    int keepAliveSec;             // PINGREQ interval; 0 = disabled
    int reconnectMinMs;           // initial reconnect backoff (default 1000)
    int reconnectMaxMs;           // reconnect backoff cap (default 30000)
    std::string topicPrefix;      // e.g. "myprot" -> myprot/<device>/<tag>
    std::string publishMode;      // "changed" = only changed values, "all" = every tick
    int publishIntervalMs;        // snapshot/publication period
    int qos;                      // 0 or 1 (QoS2 not implemented)
    bool retain;                  // MQTT retain flag on published messages

    MqttConfig()
        : enabled(false), host("127.0.0.1"), port(1883),
          clientId("myprot-gw"), keepAliveSec(60),
          reconnectMinMs(1000), reconnectMaxMs(30000),
          topicPrefix("myprot"), publishMode("changed"),
          publishIntervalMs(1000), qos(0), retain(false) {}
};

}} // namespace MyProt::Core

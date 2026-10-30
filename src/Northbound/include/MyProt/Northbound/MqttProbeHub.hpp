// src/Northbound/include/MyProt/Northbound/MqttProbeHub.hpp
// Ad-hoc MQTT observation sessions for the management plane (web UI MQTT test panel).
//
// Purpose: the browser cannot open a raw TCP 1883 connection, so the gateway
// connects on its behalf (reusing MqttClient), SUBSCRIBEs a user-supplied topic
// filter and buffers received messages. The HTTP layer drains the buffer through
// Poll() and pushes it to the browser over SSE.
//
// Thread model: one private io_context + thread (same shape as MqttPublisher).
// Create/Poll/Status/Stop/Publish are callable from any non-IO thread; every
// shared field is guarded by one mutex. Client callbacks run on the IO thread.
//
// Limits: bounded sessions, bounded event queue, bounded payload preview, idle
// reaping - the management plane must not be able to grow memory without bound.
//
// C++11 / VS2015 (ADR-0010).
#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace MyProt { namespace Northbound {

class MqttClient;

/// One observation request. host accepts "host", "host:1884", "mqtt://host:1884"
/// or "[::1]:1883" (a port embedded in host wins over the port field).
struct MqttProbeSpec {
    std::string host;
    int port;              // 0 => 1883
    std::string topic;     // subscription filter (required)
    int qos;               // 0 or 1
    std::string clientId;  // empty => "myprot-probe-<sid[0,8)>"
    std::string username;
    std::string password;
    int keepAliveSec;      // <= 0 => 60

    MqttProbeSpec() : port(0), qos(0), keepAliveSec(60) {}
};

/// Hub limits; overridable so tests can shrink timeouts and capacities.
struct MqttProbeOptions {
    int maxSessions;
    int idleTimeoutMs;        // no SSE drain for this long => session is reaped
    int statusTickMs;         // connection state sampling period
    int heartbeatMs;          // emit an empty frame at least this often
    std::size_t maxEvents;    // queued events per session (oldest dropped first)
    std::size_t maxPayloadBytes;  // payload preview cap (longer payloads are hex-truncated)

    MqttProbeOptions()
        : maxSessions(8), idleTimeoutMs(30000), statusTickMs(500),
          heartbeatMs(5000), maxEvents(500), maxPayloadBytes(8192) {}
};

class MqttProbeHub {
public:
    typedef MqttProbeOptions Options;

    explicit MqttProbeHub(const Options& opts = Options());
    ~MqttProbeHub();

    /// Starts an observation session; returns the session id, or "" and fills
    /// *error with a user-facing (Chinese) message on invalid input / capacity.
    std::string Create(const MqttProbeSpec& spec, std::string* error);

    /// Drains buffered events since the previous call.
    /// @return JSON frame, or "" when there is nothing to push this round
    ///         (an unknown sid yields a "closed" frame so the UI can reset)
    std::string Poll(const std::string& sid);

    /// Snapshot JSON; "" when the sid is unknown.
    std::string Status(const std::string& sid);

    /// Stops and forgets a session; false when the sid is unknown.
    bool Stop(const std::string& sid);

    /// Stops every session (keeps the hub usable; also safe from the IO thread).
    void StopAll();

    /// Publishes through the session's own connection (loopback self-test).
    bool Publish(const std::string& sid, const std::string& topic,
                 const std::string& payload, int qos, bool retain,
                 std::string* error);

    std::size_t Count() const;

    // ── pure helpers (exposed for validation and tests) ──

    /// MQTT 3.1.1 §4.7: '#' only as the last whole level, '+' only as a whole level.
    static bool IsValidTopicFilter(const std::string& filter);

    /// UTF-8 well-formedness (rejects overlong forms, surrogates and > U+10FFFF).
    static bool IsValidUtf8(const std::string& text);

    /// Parses "[scheme://]host[:port][/path]" into host/port; port defaults to 1883.
    static bool NormalizeBroker(std::string* host, int* port);

private:
    struct Session;

    void StartSession(const std::string& sid, const MqttProbeSpec& spec);
    void OnMessage(const std::string& sid, const std::string& topic,
                   const std::string& payload, int qos, bool retain);
    void ArmTick();
    void OnTick();
    void RemoveSession(const std::string& sid);

    Options _opts;

    mutable std::mutex _mutex;
    std::map<std::string, std::shared_ptr<Session> > _sessions;

    std::unique_ptr<asio::io_context> _io;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > _work;
    std::unique_ptr<asio::steady_timer> _tick;
    std::thread _thread;
    std::atomic<bool> _running;

    MqttProbeHub(const MqttProbeHub&) = delete;
    MqttProbeHub& operator=(const MqttProbeHub&) = delete;
};

}} // namespace MyProt::Northbound

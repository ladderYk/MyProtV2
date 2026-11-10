// src/Southbound/include/MyProt/Southbound/HttpPuller.hpp
// HTTP pull adapter (ADR-0023): the gateway is the HTTP client and issues one request per
// round. A private worker thread runs the blocking exchange; frames are posted back to the
// manager's io_context so the watchdog / sink / binding state stays single-threaded.
// The TCP connection is reused across rounds (HTTP/1.1 keep-alive); consecutive failures
// back off exponentially. v1 scope: plain http://, GET/POST, Content-Length / chunked /
// read-to-EOF. No TLS / gzip / redirects.

#pragma once
#include <asio.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Southbound/IInboundTransport.hpp"

namespace MyProt { namespace Southbound {

class HttpExchange;   // 单条可复用连接 (HttpPuller.cpp 内部实现)

class HttpPuller : public IInboundTransport,
                   public std::enable_shared_from_this<HttpPuller> {
public:
    HttpPuller(asio::io_context& io,
               const std::string& deviceId,
               const Core::HttpPullConfig& cfg);
    ~HttpPuller();

    void Start(FrameHandler onFrame, ErrorHandler onError);
    void Stop();
    bool IsRunning() const { return _running.load(); }

    /// Per-round health signal (deviceId, healthy). Invoked on the manager io_context,
    /// BEFORE the frame/error callback of the same round, so a device's lifecycle can follow
    /// pull reachability without the Southbound module depending on Service.
    typedef std::function<void(const std::string&, bool)> HealthHandler;
    void SetHealthHandler(HealthHandler handler) { _onHealth = handler; }

    /// Split "http://host[:port][/path]" (exposed for unit tests).
    /// Rejects https, credentials, empty hosts and out-of-range ports.
    static bool ParseUrl(const std::string& url,
                         std::string& host, uint16_t& port,
                         std::string& path, std::string& err);

    /// Delay before the next round (exposed for unit tests).
    /// failures == 0 -> intervalMs; otherwise exponential backoff
    /// base * 2^(failures-1), clamped to [intervalMs, backoffMaxMs].
    /// backoffBaseMs == 0 disables backoff.
    static int ComputeRetryDelayMs(int failures, int intervalMs,
                                   int backoffBaseMs, int backoffMaxMs);

    /// One-shot probe result (management-plane "trial pull").
    /// The probe opens its own connection and closes it afterwards, so it never
    /// interferes with the scheduled pull loop (and never reuses its keep-alive).
    struct ProbeResult {
        bool ok;             // transport level succeeded AND the status is 2xx
        int status;          // HTTP status code (0 = no response)
        int elapsedMs;       // wall-clock time of the whole exchange
        std::string body;    // decoded response body (only when ok)
        std::string error;   // failure reason (DNS / connect / timeout / HTTP status)
        ProbeResult() : ok(false), status(0), elapsedMs(0) {}
    };

    /// One-shot blocking GET/POST with the same request construction as the loop.
    /// MUST NOT be called on the pull worker thread: the management plane runs it
    /// on the HTTP request thread.
    static ProbeResult Probe(const Core::HttpPullConfig& cfg);

private:
    struct Endpoint {
        std::string host;
        uint16_t port;
        std::string path;
        Endpoint() : port(80) {}
    };

    void BuildRequest();
    void Loop();
    bool Exchange(const Endpoint& ep, const std::string& request,
                  std::string& body, std::string& err);
    void PostFrame(const std::string& payload);
    void PostError(const std::string& message);
    void PostHealth(bool healthy);
    bool StopRequested();

    asio::io_context& _io;
    std::string _deviceId;
    Core::HttpPullConfig _cfg;

    Endpoint _endpoint;
    std::string _request;
    std::string _urlError;

    FrameHandler _onFrame;
    ErrorHandler _onError;
    HealthHandler _onHealth;

    std::shared_ptr<HttpExchange> _exchange;
    int _reportedConnections;   // worker-thread only: last value pushed to the counter

    std::thread _thread;
    std::mutex _mutex;
    std::condition_variable _cv;
    bool _stop;
    std::atomic<bool> _running;

    HttpPuller(const HttpPuller&) = delete;
    HttpPuller& operator=(const HttpPuller&) = delete;
};

}} // namespace MyProt::Southbound

// src/WebApi/include/MyProt/WebApi/WebApiServer.hpp
// WebApi server - management-plane REST API (modules/07_WebApi.md, ADR-0008)
// Note: cpp-httplib 0.14.3 needs C++14 (incompatible with v140/C++11), so a minimal HTTP/1.1 service is
//       self-implemented on standalone Asio; TLS deferred (v1 decision); token auth + rate limiting see AuthMiddleware.

#pragma once
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <utility>
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/WebApi/AuthMiddleware.hpp"

namespace MyProt { namespace WebApi {

/// WebApi server - tag query + config CRUD + hot-reload management interface
/// Security hardening: token auth + rate limiting (ADR-0008); TLS deferred to a later version
/// Endpoints:
///   GET  /health                      health check (no auth)
///   GET  /metrics                     Prometheus metrics exposure (no auth, text/plain v0.0.4)
///   GET  /api/config/schema          field-registry delivery (feeds the UI's dynamic forms)
///   GET  /api/config/{scope}          list config items (protocols | tags)
///   GET  /api/config/{scope}/{name}   read the raw config
///   PUT  /api/config/{scope}/{name}   save config (body = raw JSON, triggers a hot reload)
///   DEL  /api/config/{scope}/{name}   delete a protocol config (protocols only)
///   GET  /api/config/{scope}/{name}/backups           list backup tags
///   POST /api/config/{scope}/{name}/backups/{tag}/restore   roll back to a specified backup
///   POST /api/config/reload           manual hot reload
///   /api/sim/*                        extension routes (injected via SetExtHandler, Phase 2 simulation data plane)
///   GET  /*                           static frontend hosting (webRoot, Vue build output)
class WebApiServer {
public:
    /// External-domain extension-route callback - dependency inversion: the App layer injects the business handler
    /// (/api/sim/*, /api/data/*); WebApi does not reverse-depend on Simulation or other modules.
    /// Returns (HTTP status, JSON body).
    typedef std::function<std::pair<int, std::string>(
        const std::string& method, const std::string& path,
        const std::string& body)> ExtHandler;

    /// SSE streaming-snapshot provider - called back once per tick, returning the JSON text to push;
    /// returning an empty string = no data to push this round (skip, do not send an empty data frame).
    /// Dependency inversion: like ExtHandler, the App layer injects the business snapshot, WebApi does not depend on business modules
    typedef std::function<std::string(const std::string& path)> SnapshotProvider;

    /// @param config management-plane security config (ConfigRoot.webApi);
    ///               bindAddress supports the "host[:port]" form, default port 8080
    /// @param store  config read/write service (ConfigStore), held by reference; its lifetime is guaranteed by the caller
    /// @throws std::runtime_error when requireAuth=true and the environment variable MYPROT_API_TOKEN is missing (Fail-Fast)
    WebApiServer(const Core::WebApiConfig& config, Service::ConfigStore& store);

    /// Register the extension-route handler (must be called before Start; when unregistered /api/sim/* -> 404)
    void SetExtHandler(ExtHandler h) { _ext = h; }

    /// Register the SSE streaming route (must be called before Start; when unregistered the streaming branch is not enabled)
    /// GET <pathPrefix>[?query] pushes the first frame immediately after connecting, then a snapshot every intervalMs;
    /// frame format "data: {json}\n\n" (text/event-stream), cleaned up on client disconnect/Stop
    /// Typical use: /api/data/stream real-time tag snapshots (modules/07_WebApi.md)
    void SetStreamRoute(const std::string& pathPrefix, int intervalMs,
                        SnapshotProvider fn) {
        _streamPrefix = pathPrefix;
        _streamIntervalMs = intervalMs;
        _streamProvider = fn;
    }

    /// Start the HTTP server and block on listening (internally io_context.run(); the caller decides the thread)
    void Start();

    /// Stop the server (thread-safe, can be called from another thread)
    void Stop();

private:
    /// HTTP response
    struct Response {
        int status;
        std::string contentType;
        std::string body;
        bool cacheable;     // static resources true (Vite hashed file names can be safely cached); API always no-store

        Response() : status(200), cacheable(false) {}
        Response(int s, const std::string& ct, const std::string& b, bool c = false)
            : status(s), contentType(ct), body(b), cacheable(c) {}
    };

    /// Route dispatch (parsed request -> response)
    Response Route(const std::string& method,
                   const std::string& path,
                   const std::string& body,
                   const std::string& authToken);

    /// Static file serving (under webRoot; guards against path traversal; sets Content-Type by extension)
    Response ServeStatic(const std::string& path);

    /// Route + serialize + async write-back + close connection
    void FinishRequest(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                       const std::string& method,
                       const std::string& path,
                       const std::string& authToken,
                       const std::string& body);

    /// Directly send a simple response and close the connection (malformed request / over-limit etc., no routing needed)
    void RespondAndClose(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                         int status, const std::string& contentType,
                         const std::string& body);

    void DoAccept();
    void HandleConnection(const std::shared_ptr<asio::ip::tcp::socket>& socket);

    /// SSE streaming session state bundle - held by the async callback chain for the connection's life (socket+timer live and die together)
    struct StreamSession {
        StreamSession(asio::io_context& io, const std::string& p)
            : timer(new asio::steady_timer(io)), path(p) {}
        std::shared_ptr<asio::ip::tcp::socket> socket;   // filled back at handshake
        std::shared_ptr<asio::steady_timer> timer;
        std::string path;                                // includes query (device/token)
        std::shared_ptr<std::string> payload;            // in-flight frame buffer (kept alive until written)
    };

    /// SSE handshake: rate-limit + auth (with ?token= fallback) -> response headers + first frame -> enter the periodic-push loop
    void HandleStream(const std::shared_ptr<StreamSession>& session,
                      const std::string& authToken);
    /// Wait for the next push cycle (close the session on Stop/cancel)
    void StreamWait(const std::shared_ptr<StreamSession>& session);
    /// Push one snapshot frame (a provider exception/write failure disconnects; an empty snapshot skips this round)
    void StreamPush(const std::shared_ptr<StreamSession>& session);
    /// Close an SSE session (cancel timer + shutdown socket)
    static void StreamClose(const std::shared_ptr<StreamSession>& session);

    Core::WebApiConfig _config;
    Service::ConfigStore& _store;
    std::string _token;             // from MYPROT_API_TOKEN; must not be empty when requireAuth
    AuthMiddleware _auth;
    RateLimiter _limiter;
    ExtHandler _ext;
    SnapshotProvider _streamProvider;
    std::string _streamPrefix;      // SSE path prefix (when unregistered _streamProvider is empty)
    int _streamIntervalMs;
    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    std::atomic<bool> _running;
};

}} // namespace MyProt::WebApi

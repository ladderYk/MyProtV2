// src/WebApi/include/MyProt/WebApi/WebApiServer.hpp
// WebApi 服务器 — 管理面 REST API (modules/07_WebApi.md, ADR-0008)
// 说明: cpp-httplib 0.14.3 需 C++14 (与 v140/C++11 不兼容), 故基于 standalone Asio
//       自实现最小 HTTP/1.1 服务; TLS 暂缓 (v1 决策), token 认证 + 限流见 AuthMiddleware。

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

/// WebApi 服务器 — 标签查询 + 配置 CRUD + 热重载管理接口
/// 安全加固: token 认证 + 限流 (ADR-0008); TLS 待后续版本
/// 端点:
///   GET  /health                      健康检查 (免认证)
///   GET  /metrics                     Prometheus 指标暴露 (免认证, text/plain v0.0.4)
///   GET  /api/config/schema          字段注册表下发 (UI 动态表单供数)
///   GET  /api/config/{scope}          列出配置项 (protocols | tags)
///   GET  /api/config/{scope}/{name}   读取配置原文
///   PUT  /api/config/{scope}/{name}   保存配置 (body = JSON 原文, 触发热重载)
///   DEL  /api/config/{scope}/{name}   删除协议配置 (仅 protocols)
///   GET  /api/config/{scope}/{name}/backups           列出备份标签
///   POST /api/config/{scope}/{name}/backups/{tag}/restore   回滚到指定备份
///   POST /api/config/reload           手动热重载
///   /api/sim/*                        扩展路由 (SetExtHandler 注入, Phase 2 仿真数据面)
///   GET  /*                           静态前端托管 (webRoot, Vue 构建产物)
class WebApiServer {
public:
    /// 外部域扩展路由回调 — 依赖倒置: App 层注入业务 handler
    /// (/api/sim/*、/api/data/*), WebApi 不反向依赖 Simulation 等模块。
    /// 返回 (HTTP status, JSON body)。
    typedef std::function<std::pair<int, std::string>(
        const std::string& method, const std::string& path,
        const std::string& body)> ExtHandler;

    /// SSE 流式快照提供者 — 每 tick 回调一次, 返回待推送的 JSON 文本;
    /// 返回空串 = 本轮无数据可推 (跳过, 不发空 data 帧)。
    /// 依赖倒置: 与 ExtHandler 一致, App 层注入业务快照, WebApi 不依赖业务模块
    typedef std::function<std::string(const std::string& path)> SnapshotProvider;

    /// @param config 管理面安全配置 (ConfigRoot.webApi);
    ///               bindAddress 支持 "host[:port]" 形式, 缺省端口 8080
    /// @param store  配置读写服务 (ConfigStore), 持有引用, 生命周期由调用方保证
    /// @throws std::runtime_error 当 requireAuth=true 且环境变量 MYPROT_API_TOKEN 缺失时 Fail-Fast
    WebApiServer(const Core::WebApiConfig& config, Service::ConfigStore& store);

    /// 注册扩展路由 handler (须在 Start 前调用; 未注册时 /api/sim/* → 404)
    void SetExtHandler(ExtHandler h) { _ext = h; }

    /// 注册 SSE 流式路由 (须在 Start 前调用; 未注册时不启用流式分支)
    /// GET <pathPrefix>[?query] 建连后立即推首帧, 此后每 intervalMs 推一次快照;
    /// 帧格式 "data: {json}\n\n" (text/event-stream), 客户端断开/Stop 时清理
    /// 典型用途: /api/data/stream 实时标签快照 (modules/07_WebApi.md)
    void SetStreamRoute(const std::string& pathPrefix, int intervalMs,
                        SnapshotProvider fn) {
        _streamPrefix = pathPrefix;
        _streamIntervalMs = intervalMs;
        _streamProvider = fn;
    }

    /// 启动 HTTP 服务器并阻塞监听 (内部 io_context.run(); 由调用方决定线程)
    void Start();

    /// 停止服务器 (线程安全, 可从其他线程调用)
    void Stop();

private:
    /// HTTP 响应
    struct Response {
        int status;
        std::string contentType;
        std::string body;
        bool cacheable;     // 静态资源 true (Vite 文件名带 hash 可安全缓存); API 一律 no-store

        Response() : status(200), cacheable(false) {}
        Response(int s, const std::string& ct, const std::string& b, bool c = false)
            : status(s), contentType(ct), body(b), cacheable(c) {}
    };

    /// 路由分发 (已解析请求 → 响应)
    Response Route(const std::string& method,
                   const std::string& path,
                   const std::string& body,
                   const std::string& authToken);

    /// 静态文件服务 (webRoot 下; 防路径穿越; 按扩展名给 Content-Type)
    Response ServeStatic(const std::string& path);

    /// 路由 + 序列化 + 异步写回 + 关闭连接
    void FinishRequest(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                       const std::string& method,
                       const std::string& path,
                       const std::string& authToken,
                       const std::string& body);

    /// 直接发送简单响应并关闭连接 (畸形请求 / 超限等无需路由的场景)
    void RespondAndClose(const std::shared_ptr<asio::ip::tcp::socket>& socket,
                         int status, const std::string& contentType,
                         const std::string& body);

    void DoAccept();
    void HandleConnection(const std::shared_ptr<asio::ip::tcp::socket>& socket);

    /// SSE 流式会话状态包 — 连接存续期由异步回调链持有 (socket+timer 同生共死)
    struct StreamSession {
        StreamSession(asio::io_context& io, const std::string& p)
            : timer(new asio::steady_timer(io)), path(p) {}
        std::shared_ptr<asio::ip::tcp::socket> socket;   // 握手时回填
        std::shared_ptr<asio::steady_timer> timer;
        std::string path;                                // 含 query (device/token)
        std::shared_ptr<std::string> payload;            // 在途帧缓冲 (保活至写完)
    };

    /// SSE 握手: 限流+认证(含 ?token= 兜底) → 响应头+首帧 → 进入周期推送循环
    void HandleStream(const std::shared_ptr<StreamSession>& session,
                      const std::string& authToken);
    /// 等待下一推送周期 (Stop/取消时关闭会话)
    void StreamWait(const std::shared_ptr<StreamSession>& session);
    /// 推送一帧快照 (provider 异常/写失败即断开; 空快照跳过本轮)
    void StreamPush(const std::shared_ptr<StreamSession>& session);
    /// 关闭 SSE 会话 (cancel timer + shutdown socket)
    static void StreamClose(const std::shared_ptr<StreamSession>& session);

    Core::WebApiConfig _config;
    Service::ConfigStore& _store;
    std::string _token;             // 来自 MYPROT_API_TOKEN; requireAuth 时不得为空
    AuthMiddleware _auth;
    RateLimiter _limiter;
    ExtHandler _ext;
    SnapshotProvider _streamProvider;
    std::string _streamPrefix;      // SSE 路径前缀 (未注册时 _streamProvider 为空)
    int _streamIntervalMs;
    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    std::atomic<bool> _running;
};

}} // namespace MyProt::WebApi

// src/WebApi/src/WebApiServer.cpp — WebApi 服务器实现 (standalone Asio 最小 HTTP/1.1)
// cpp-httplib 0.14.3 需 C++14, 与 v140/C++11 不兼容, 故自实现;
// 完整路由设计见 modules/07_WebApi.md + design-proposal/v2-config-ui-design。

#include "MyProt/WebApi/WebApiServer.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Service/SchemaRegistry.hpp"
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <sstream>
#include <fstream>

namespace MyProt { namespace WebApi {

namespace {

const char* kTokenEnvVar = "MYPROT_API_TOKEN";
const char* kDefaultPort = "8080";
const size_t kMaxBodySize = 1u * 1024u * 1024u;      // 请求体上限 1MB

std::string ReasonPhrase(int code) {
    switch (code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default:  return "Unknown";
    }
}

/// 字符串 → JSON 字符串字面量 (含引号)
std::string JsonString(const std::string& s) {
    std::string out = "\"";
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    out += "\"";
    return out;
}

std::string JsonArray(const std::vector<std::string>& items) {
    std::string out = "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ",";
        out += JsonString(items[i]);
    }
    out += "]";
    return out;
}

/// 头部名等值比较 (大小写不敏感)
bool HeaderIs(const std::string& name, const char* canonicalLower) {
    if (name.size() != std::strlen(canonicalLower)) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(name[i])) != canonicalLower[i]) {
            return false;
        }
    }
    return true;
}

/// 路径按 '/' 分段
std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::istringstream iss(path);
    std::string seg;
    while (std::getline(iss, seg, '/')) {
        if (!seg.empty()) parts.push_back(seg);
    }
    return parts;
}

bool ScopeFromString(const std::string& s, Service::ConfigScope* out) {
    if (s == "protocols") { *out = Service::ConfigScope::Protocol; return true; }
    if (s == "tags")      { *out = Service::ConfigScope::Tags;     return true; }
    return false;
}

/// Core 错误码 → HTTP 状态码
int StatusFromError(const Core::Error::Code code) {
    switch (code) {
        case Core::Error::Code::DeviceNotFound:
        case Core::Error::Code::ProtocolNotFound:
        case Core::Error::Code::TagNotFound:
            return 404;
        case Core::Error::Code::ParseError:
        case Core::Error::Code::ConfigError:
            return 400;
        case Core::Error::Code::Busy:
        case Core::Error::Code::CircuitOpen:
            return 503;
        default:
            return 500;
    }
}

/// 静态文件扩展名 → Content-Type (覆盖 Vite 构建产物所需集合)
const char* MimeFromExtension(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const std::string ext = path.substr(dot + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js")                   return "application/javascript; charset=utf-8";
    if (ext == "css")                  return "text/css; charset=utf-8";
    if (ext == "json" || ext == "map") return "application/json; charset=utf-8";
    if (ext == "svg")                  return "image/svg+xml";
    if (ext == "png")                  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")                  return "image/gif";
    if (ext == "ico")                  return "image/x-icon";
    if (ext == "woff")                 return "font/woff";
    if (ext == "woff2")                return "font/woff2";
    if (ext == "ttf")                  return "font/ttf";
    if (ext == "txt")                  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

/// 读取静态文件全文; 不存在/不可读返回 false
bool ReadFileContent(const std::string& path, std::string* out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return true;
}

/// 读取环境变量 (getenv_s 安全版, SDL 下 getenv 触发 C4996)
std::string ReadEnvValue(const char* name) {
    size_t required = 0;
    if (getenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
        return std::string();
    }
    std::vector<char> buf(required);
    size_t written = 0;
    if (getenv_s(&written, &buf[0], required, name) != 0) {
        return std::string();
    }
    return std::string(&buf[0], written > 0 ? written - 1 : 0);   // 去掉结尾 '\0'
}

/// 从 "/path?key=v&..." 提取 query 参数值 (含最小 %XX 解码; 未找到返回空串)
std::string QueryParamValue(const std::string& path, const char* key) {
    const size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    const std::string needle = std::string(key) + "=";
    size_t pos = q + 1;
    while (pos <= path.size()) {
        const size_t amp = path.find('&', pos);
        const std::string pairStr = path.substr(pos,
            (amp == std::string::npos) ? std::string::npos : amp - pos);
        if (pairStr.compare(0, needle.size(), needle) == 0) {
            const std::string val = pairStr.substr(needle.size());
            std::string out;                              // 最小 %XX 解码
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '%' && i + 2 < val.size()
                        && std::isxdigit(static_cast<unsigned char>(val[i + 1]))
                        && std::isxdigit(static_cast<unsigned char>(val[i + 2]))) {
                    out += static_cast<char>(
                        std::strtol(val.substr(i + 1, 2).c_str(), nullptr, 16));
                    i += 2;
                } else {
                    out += val[i];
                }
            }
            return out;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

} // namespace

// ────────── 生命周期 ──────────

WebApiServer::WebApiServer(const Core::WebApiConfig& config, Service::ConfigStore& store)
    : _config(config), _store(store)
    , _token(ReadEnvValue(kTokenEnvVar)), _auth(_token)
    , _limiter(config.rateLimitRps, config.rateLimitBurst)
    , _io(), _acceptor(_io), _running(false)
    , _streamIntervalMs(3000) {
    // Fail-Fast: 生产模式要求显式配置 token (ADR-0008 §3)
    if (_config.requireAuth && _token.empty()) {
        throw std::runtime_error("WebApi: MYPROT_API_TOKEN 未设置但 webApi.requireAuth=true");
    }
}

void WebApiServer::Start() {
    // bindAddress 支持 "host[:port]"
    std::string host = _config.bindAddress.empty() ? "127.0.0.1" : _config.bindAddress;
    std::string portStr = kDefaultPort;
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {   // IPv4 host:port
        portStr = host.substr(colon + 1);
        host = host.substr(0, colon);
    }

    asio::ip::tcp::resolver resolver(_io);
    asio::ip::tcp::resolver::results_type endpoints =
        resolver.resolve(host, portStr);

    _acceptor.open(endpoints.begin()->endpoint().protocol());
    _acceptor.set_option(asio::socket_base::reuse_address(true));
    _acceptor.bind(endpoints.begin()->endpoint());
    _acceptor.listen(asio::socket_base::max_listen_connections);

    _running.store(true);
    DoAccept();
    _io.run();                                            // 阻塞监听
}

void WebApiServer::Stop() {
    _running.store(false);
    _io.stop();                                           // io_context::stop 线程安全
}

// ────────── 连接处理 ──────────

void WebApiServer::DoAccept() {
    std::shared_ptr<asio::ip::tcp::socket> socket(new asio::ip::tcp::socket(_io));
    _acceptor.async_accept(*socket,
        [this, socket](const asio::error_code& ec) {
            if (!ec && _running.load()) {
                HandleConnection(socket);
            }
            if (_running.load()) DoAccept();              // 继续接受下一连接
        });
}

void WebApiServer::HandleConnection(
        const std::shared_ptr<asio::ip::tcp::socket>& socket) {
    auto buf = std::shared_ptr<asio::streambuf>(new asio::streambuf());

    asio::async_read_until(*socket, *buf, "\r\n\r\n",
        [this, socket, buf](const asio::error_code& ec, size_t /*bytes*/) {
            if (ec) return;

            std::istream is(buf.get());

            // ── 请求行 ──
            std::string requestLine;
            std::getline(is, requestLine);
            if (!requestLine.empty() && requestLine[requestLine.size() - 1] == '\r') {
                requestLine.erase(requestLine.size() - 1);
            }
            const size_t sp1 = requestLine.find(' ');
            const size_t sp2 = (sp1 == std::string::npos)
                ? std::string::npos : requestLine.find(' ', sp1 + 1);
            if (sp1 == std::string::npos || sp2 == std::string::npos) {
                RespondAndClose(socket, 400, "text/plain; charset=utf-8", "bad request");
                return;
            }
            const std::string method = requestLine.substr(0, sp1);
            const std::string path   = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

            // ── 头部 ──
            size_t contentLength = 0;
            std::string authToken;
            std::string headerLine;
            bool malformed = false;
            while (std::getline(is, headerLine)) {
                if (!headerLine.empty() && headerLine[headerLine.size() - 1] == '\r') {
                    headerLine.erase(headerLine.size() - 1);
                }
                if (headerLine.empty()) break;            // 头结束
                const size_t colonPos = headerLine.find(':');
                if (colonPos == std::string::npos) continue;
                const std::string name = headerLine.substr(0, colonPos);
                std::string value = headerLine.substr(colonPos + 1);
                const size_t vbegin = value.find_first_not_of(" \t");
                value = (vbegin == std::string::npos) ? "" : value.substr(vbegin);

                if (HeaderIs(name, "content-length")) {
                    contentLength = static_cast<size_t>(_atoi64(value.c_str()));
                    if (contentLength > kMaxBodySize) malformed = true;
                } else if (HeaderIs(name, "authorization")) {
                    if (value.size() > 7 && value.compare(0, 7, "Bearer ") == 0) {
                        authToken = value.substr(7);
                    }
                }
            }

            if (malformed) {                              // 413 → 写回后关闭
                RespondAndClose(socket, 413, "application/json",
                                "{\"error\":\"payload too large\"}");
                return;
            }

            // ── SSE 流式路由 (独立于 Connection: close 模型; 仅 GET, 无 body) ──
            if (_streamProvider && method == "GET" &&
                    path.size() >= _streamPrefix.size() &&
                    path.compare(0, _streamPrefix.size(), _streamPrefix) == 0 &&
                    (path.size() == _streamPrefix.size()
                        || path[_streamPrefix.size()] == '?')) {
                auto session = std::shared_ptr<StreamSession>(
                    new StreamSession(_io, path));
                session->socket = socket;
                HandleStream(session, authToken);
                return;
            }

            // ── Body (streambuf 中可能已含部分) ──
            auto bodyStr = std::shared_ptr<std::string>(new std::string());
            const size_t buffered = static_cast<size_t>(buf->in_avail());
            const size_t take = (buffered < contentLength) ? buffered : contentLength;
            if (take > 0) {
                bodyStr->resize(take);
                is.read(&(*bodyStr)[0], static_cast<std::streamsize>(take));
            }

            if (bodyStr->size() < contentLength) {        // 继续读剩余 body
                const size_t remaining = contentLength - bodyStr->size();
                const size_t oldSize = bodyStr->size();
                bodyStr->resize(contentLength);
                asio::async_read(*socket,
                    asio::buffer(&(*bodyStr)[oldSize], remaining),
                    [this, socket, bodyStr, method, path, authToken]
                        (const asio::error_code& ec2, size_t) {
                        if (ec2) return;
                        FinishRequest(socket, method, path, authToken, *bodyStr);
                    });
            } else {
                FinishRequest(socket, method, path, authToken, *bodyStr);
            }
        });
}

/// v1.14 增: JSON 响应统一补 charset=utf-8。
///   无 charset 时部分客户端 (PowerShell 5.1、老脚本/第三方集成) 会按 Latin-1 解码中文,
///   再把结果写回 → 双重编码乱码 (2026-09-13 事故的机制)。浏览器不受影响, 但接口要自描述。
static std::string WithUtf8Charset(const std::string& contentType) {
    if (contentType == "application/json") return "application/json; charset=utf-8";
    return contentType;
}

void WebApiServer::FinishRequest(
        const std::shared_ptr<asio::ip::tcp::socket>& socket,
        const std::string& method, const std::string& path,
        const std::string& authToken, const std::string& body) {
    const Response resp = Route(method, path, body, authToken);

    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status << " " << ReasonPhrase(resp.status)
        << "\r\nContent-Type: " << WithUtf8Charset(resp.contentType)
        << "\r\nContent-Length: " << resp.body.size()
        << "\r\nX-Content-Type-Options: nosniff"
        << "\r\nCache-Control: "
        << (resp.cacheable ? "public, max-age=3600" : "no-store")
        << "\r\nConnection: close\r\n\r\n";
    if (method != "HEAD") oss << resp.body;               // HEAD 只回头部
    std::shared_ptr<std::string> payload(new std::string(oss.str()));

    asio::async_write(*socket, asio::buffer(payload->data(), payload->size()),
        [socket, payload](const asio::error_code&, size_t) {
            asio::error_code ignore;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
            socket->close(ignore);
        });
}

void WebApiServer::RespondAndClose(
        const std::shared_ptr<asio::ip::tcp::socket>& socket,
        int status, const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << ReasonPhrase(status)
        << "\r\nContent-Type: " << contentType
        << "\r\nContent-Length: " << body.size()
        << "\r\nX-Content-Type-Options: nosniff"
        << "\r\nConnection: close\r\n\r\n" << body;
    std::shared_ptr<std::string> payload(new std::string(oss.str()));
    asio::async_write(*socket, asio::buffer(payload->data(), payload->size()),
        [socket, payload](const asio::error_code&, size_t) {
            asio::error_code ignore;
            socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
            socket->close(ignore);
        });
}

// ────────── SSE 流式推送 (modules/07_WebApi.md /api/data/stream) ──────────

void WebApiServer::HandleStream(const std::shared_ptr<StreamSession>& session,
                                const std::string& authToken) {
    // 限流 + 认证只在握手时判定一次; EventSource 无法自定义请求头,
    // 故 requireAuth 时允许 ?token= 查询参数兜底
    if (!_limiter.TryAcquire()) {
        RespondAndClose(session->socket, 429, "application/json",
                        "{\"error\":\"rate limit exceeded\"}");
        return;
    }
    bool authed = !_config.requireAuth || _auth.Validate(authToken);
    if (!authed) {
        const std::string qToken = QueryParamValue(session->path, "token");
        authed = !qToken.empty() && _auth.Validate(qToken);
    }
    if (!authed) {
        RespondAndClose(session->socket, 401, "application/json",
                        "{\"error\":\"unauthorized\"}");
        return;
    }

    // 响应头 + 首帧一次写出; 收到合法 200 后浏览器 EventSource 才触发 onopen
    std::string first;
    try {
        first = _streamProvider(session->path);
    } catch (const std::exception&) {
        RespondAndClose(session->socket, 500, "application/json",
                        "{\"error\":\"internal error\"}");
        return;
    }
    std::string out =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
        "Connection: keep-alive\r\n\r\n";
    out += first.empty() ? ": init\n\n" : ("data: " + first + "\n\n");
    session->payload.reset(new std::string(out));

    asio::async_write(*session->socket,
        asio::buffer(session->payload->data(), session->payload->size()),
        [this, session](const asio::error_code& ec, size_t) {
            if (ec) { StreamClose(session); return; }
            StreamWait(session);
        });
}

void WebApiServer::StreamWait(const std::shared_ptr<StreamSession>& session) {
    if (!_running.load()) { StreamClose(session); return; }
    session->timer->expires_after(std::chrono::milliseconds(_streamIntervalMs));
    session->timer->async_wait([this, session](const asio::error_code& ec) {
        // 取消 (Stop → timer.cancel / io 停止分发) 即断开清理
        if (ec) { StreamClose(session); return; }
        StreamPush(session);
    });
}

void WebApiServer::StreamPush(const std::shared_ptr<StreamSession>& session) {
    std::string snapshot;
    try {
        snapshot = _streamProvider(session->path);
    } catch (const std::exception&) {
        StreamClose(session);                 // provider 异常 → 断开, 客户端自动重连
        return;
    }
    if (snapshot.empty()) {                   // 无数据本轮跳过, 不发空 data 帧
        StreamWait(session);
        return;
    }
    session->payload.reset(new std::string("data: " + snapshot + "\n\n"));
    asio::async_write(*session->socket,
        asio::buffer(session->payload->data(), session->payload->size()),
        [this, session](const asio::error_code& ec, size_t) {
            if (ec) { StreamClose(session); return; }   // 典型: 客户端断开
            StreamWait(session);
        });
}

void WebApiServer::StreamClose(const std::shared_ptr<StreamSession>& session) {
    asio::error_code ignore;
    session->timer->cancel(ignore);
    session->socket->shutdown(asio::ip::tcp::socket::shutdown_both, ignore);
    session->socket->close(ignore);
}

// ────────── 路由 ──────────

WebApiServer::Response WebApiServer::Route(const std::string& method,
                                           const std::string& path,
                                           const std::string& body,
                                           const std::string& authToken) {
    using Service::ConfigScope;

    // ── /health 免认证 (存活探针) ──
    if (path == "/health") {
        if (method != "GET") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        return Response(200, "application/json", "{\"status\":\"ok\"}");
    }

    // ── /metrics 免认证 (Prometheus 抓取; architecture/05 可观测性) ──
    if (path == "/metrics") {
        if (method != "GET") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        return Response(200,
                        "text/plain; version=0.0.4; charset=utf-8",
                        Core::MetricsRegistry::Instance().RenderPrometheus());
    }

    // ── 静态前端托管: 免认证免限流 ──
    // 浏览器须在无 token 时加载登录页; 保护面是 API, 静态 JS/CSS 为公开资产
    const std::vector<std::string> seg = SplitPath(path);
    if (seg.empty() || seg[0] != "api") {
        if (method == "GET" || method == "HEAD") {
            return ServeStatic(path);
        }
        return Response(404, "application/json", "{\"error\":\"not found\"}");
    }

    // ── 限流 (仅 API; ADR-0008 §4) ──
    if (!_limiter.TryAcquire()) {
        return Response(429, "application/json", "{\"error\":\"rate limit exceeded\"}");
    }

    // ── token 认证 ──
    if (_config.requireAuth && !_auth.Validate(authToken)) {
        return Response(401, "application/json", "{\"error\":\"unauthorized\"}");
    }

    // ── /api/sim/*、/api/data/* → 扩展路由 (App 层注入的业务 handler) ──
    if (seg.size() >= 2 && seg[0] == "api" &&
        (seg[1] == "sim" || seg[1] == "data")) {
        if (!_ext) {
            return Response(404, "application/json",
                            "{\"error\":\"sim api not available\"}");
        }
        try {
            const std::pair<int, std::string> r = _ext(method, path, body);
            return Response(r.first, "application/json", r.second);
        } catch (const std::exception&) {
            return Response(500, "application/json",
                            "{\"error\":\"internal error\"}");
        }
    }

    // ── /api/config/reload (POST) ──
    if (seg.size() == 3 && seg[0] == "api" && seg[1] == "config" && seg[2] == "reload") {
        if (method != "POST") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        auto res = _store.Reload();
        if (!res.has_value()) {
            const Core::Error& err = res.error();
            return Response(StatusFromError(err.code), "application/json",
                            "{\"error\":" + JsonString(err.message) + "}");
        }
        return Response(200, "application/json", "{\"ok\":true}");
    }

    // ── /api/config/schema (GET=字段注册表下发, UI 动态表单供数) ──
    if (seg.size() == 3 && seg[0] == "api" && seg[1] == "config" && seg[2] == "schema") {
        if (method != "GET") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        return Response(200, "application/json",
                        Service::SchemaRegistry::ToJson(_store.SupportedSchemaVersion()));
    }

    // ── /api/config/{scope} (GET=List) ──
    if (seg.size() == 3 && seg[0] == "api" && seg[1] == "config") {
        ConfigScope scope;
        if (!ScopeFromString(seg[2], &scope)) {
            return Response(404, "application/json", "{\"error\":\"unknown scope\"}");
        }
        if (method != "GET") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        auto res = _store.List(scope);
        if (!res.has_value()) {
            const Core::Error& err = res.error();
            return Response(StatusFromError(err.code), "application/json",
                            "{\"error\":" + JsonString(err.message) + "}");
        }
        return Response(200, "application/json", JsonArray(res.value()));
    }

    // ── /api/config/{scope}/{name} (GET=读 / PUT=写 / DELETE=删) ──
    if (seg.size() == 4 && seg[0] == "api" && seg[1] == "config") {
        ConfigScope scope;
        if (!ScopeFromString(seg[2], &scope)) {
            return Response(404, "application/json", "{\"error\":\"unknown scope\"}");
        }
        const std::string name = seg[3];

        if (method == "GET") {
            auto res = _store.Get(scope, name);
            if (!res.has_value()) {
                const Core::Error& err = res.error();
                return Response(StatusFromError(err.code), "application/json",
                                "{\"error\":" + JsonString(err.message) + "}");
            }
            return Response(200, "application/json", res.value());
        }

        if (method == "PUT") {                            // Save: 校验→备份→原子写→热重载
            auto res = _store.Save(scope, name, body);
            if (!res.has_value()) {
                const Core::Error& err = res.error();
                return Response(StatusFromError(err.code), "application/json",
                                "{\"error\":" + JsonString(err.message) + "}");
            }
            return Response(200, "application/json", "{\"ok\":true}");
        }

        if (method == "DELETE") {                         // Delete: 删除文件+备份
            auto res = _store.Delete(scope, name);
            if (!res.has_value()) {
                const Core::Error& err = res.error();
                return Response(StatusFromError(err.code), "application/json",
                                "{\"error\":" + JsonString(err.message) + "}");
            }
            return Response(200, "application/json", "{\"ok\":true}");
        }

        return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
    }

    // ── /api/config/{scope}/{name}/backups (GET=备份列表) ──
    if (seg.size() == 5 && seg[0] == "api" && seg[1] == "config"
            && seg[4] == "backups" && method == "GET") {
        ConfigScope scope;
        if (!ScopeFromString(seg[2], &scope)) {
            return Response(404, "application/json", "{\"error\":\"unknown scope\"}");
        }
        auto res = _store.ListBackups(scope, seg[3]);
        if (!res.has_value()) {
            const Core::Error& err = res.error();
            return Response(StatusFromError(err.code), "application/json",
                            "{\"error\":" + JsonString(err.message) + "}");
        }
        return Response(200, "application/json", JsonArray(res.value()));
    }

    // ── /api/config/{scope}/{name}/backups/{tag}/restore (POST=回滚) ──
    if (seg.size() == 7 && seg[0] == "api" && seg[1] == "config"
            && seg[4] == "backups" && seg[6] == "restore") {
        if (method != "POST") {
            return Response(405, "application/json", "{\"error\":\"method not allowed\"}");
        }
        ConfigScope scope;
        if (!ScopeFromString(seg[2], &scope)) {
            return Response(404, "application/json", "{\"error\":\"unknown scope\"}");
        }
        auto res = _store.Rollback(scope, seg[3], seg[5]);
        if (!res.has_value()) {
            const Core::Error& err = res.error();
            return Response(StatusFromError(err.code), "application/json",
                            "{\"error\":" + JsonString(err.message) + "}");
        }
        return Response(200, "application/json", "{\"ok\":true}");
    }

    // ── 未匹配的 API 路径 ──
    return Response(404, "application/json", "{\"error\":\"not found\"}");
}

// ────────── 静态文件服务 ──────────

WebApiServer::Response WebApiServer::ServeStatic(const std::string& path) {
    if (_config.webRoot.empty()) {
        return Response(404, "text/plain; charset=utf-8", "not found");
    }

    // 规范化: 仅允许 '/' 分隔的相对路径; 拒绝 ".."、绝对路径与可疑字符 (防路径穿越)
    std::string rel = path;
    const size_t query = rel.find('?');
    if (query != std::string::npos) rel = rel.substr(0, query);
    if (rel.empty() || rel[0] != '/') rel = "/" + rel;
    if (rel.find("..") != std::string::npos || rel.find('\\') != std::string::npos
            || rel.find('%') != std::string::npos) {
        return Response(400, "text/plain; charset=utf-8", "bad request");
    }
    if (rel == "/") rel = "/index.html";                  // SPA 入口

    std::string content;
    const std::string full = _config.webRoot + rel;
    if (!ReadFileContent(full, &content)) {
        return Response(404, "text/plain; charset=utf-8", "not found");
    }
    // index.html 引用带 hash 的资产, 必须每次取新; 资产文件名含内容 hash 可安全缓存
    return Response(200, MimeFromExtension(rel), content, rel != "/index.html");
}

}} // namespace MyProt::WebApi

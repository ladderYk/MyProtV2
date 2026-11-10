// src/Southbound/src/HttpPuller.cpp

#include "MyProt/Southbound/HttpPuller.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <vector>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Southbound/TagValueSink.hpp"

namespace MyProt { namespace Southbound {

namespace {

const std::size_t kMaxResponseBytes = 1024u * 1024u;
const std::size_t kMaxHeadBytes = 64u * 1024u;
// Chunked framing adds a size line + CRLF per chunk; keep a looser raw-buffer cap
// than the decoded body cap so a legitimate 1MB body made of small chunks passes.
const std::size_t kMaxRawBufferBytes = 2u * kMaxResponseBytes + kMaxHeadBytes;
const std::size_t kReadChunkBytes = 8192;

std::string LowerAscii(const std::string& s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] = static_cast<char>(out[i] - 'A' + 'a');
        }
    }
    return out;
}

bool IsControlledHeader(const std::string& lowerName) {
    return lowerName == "host" || lowerName == "connection"
        || lowerName == "content-length" || lowerName == "transfer-encoding"
        || lowerName == "accept-encoding";
}

/// Build the HTTP/1.1 request bytes. Shared by the scheduled pull loop and the
/// one-shot Probe so the management plane previews exactly what the gateway sends.
void BuildHttpRequest(const Core::HttpPullConfig& cfg,
                      const std::string& host, uint16_t port,
                      const std::string& path, std::string& out) {
    std::ostringstream head;
    head << cfg.method << " " << path << " HTTP/1.1\r\n";
    head << "Host: " << host;
    if (port != 80) head << ":" << port;
    head << "\r\n";
    head << "User-Agent: MyProt/1.0\r\n";
    head << "Accept: application/json\r\n";
    head << "Accept-Encoding: identity\r\n";
    head << "Connection: " << (cfg.keepAlive ? "keep-alive" : "close") << "\r\n";

    bool hasContentType = false;
    for (std::map<std::string, std::string>::const_iterator it = cfg.headers.begin();
            it != cfg.headers.end(); ++it) {
        const std::string lower = LowerAscii(it->first);
        if (IsControlledHeader(lower)) continue;   // framing headers stay client-owned
        if (lower == "content-type") hasContentType = true;
        head << it->first << ": " << it->second << "\r\n";
    }

    if (cfg.method == "POST") {
        if (!hasContentType) head << "Content-Type: application/json\r\n";
        head << "Content-Length: " << cfg.body.size() << "\r\n";
    }
    head << "\r\n";

    out = head.str();
    if (cfg.method == "POST") out += cfg.body;
}

/// Parse a response head block (without the terminating CRLFCRLF).
/// contentLength < 0 = not declared (chunked, or read to EOF).
/// http10 / connClose / connKeepAlive drive the keep-alive reuse decision.
bool ParseHead(const std::string& head, int& status,
               long long& contentLength, bool& chunked,
               bool& http10, bool& connClose, bool& connKeepAlive,
               std::string& err) {
    status = 0;
    contentLength = -1;
    chunked = false;
    http10 = false;
    connClose = false;
    connKeepAlive = false;

    std::size_t lineEnd = head.find("\r\n");
    const std::string statusLine = head.substr(0, lineEnd);
    if (statusLine.compare(0, 5, "HTTP/") != 0) {
        err = "响应不是 HTTP 报文";
        return false;
    }
    // HTTP/1.0 默认短连接: 只有显式 keep-alive 才允许复用.
    if (statusLine.size() >= 8 && statusLine.compare(0, 8, "HTTP/1.0") == 0) {
        http10 = true;
    }
    const std::size_t sp = statusLine.find(' ');
    if (sp == std::string::npos) {
        err = "响应状态行非法";
        return false;
    }
    status = std::atoi(statusLine.c_str() + sp + 1);
    if (status < 100 || status > 599) {
        err = "响应状态码非法";
        return false;
    }

    std::size_t pos = (lineEnd == std::string::npos) ? head.size() : lineEnd + 2;
    while (pos < head.size()) {
        std::size_t end = head.find("\r\n", pos);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(pos, end - pos);
        pos = (end == head.size()) ? head.size() : end + 2;
        if (line.empty()) continue;

        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string name = LowerAscii(line.substr(0, colon));
        std::size_t vbegin = colon + 1;
        while (vbegin < line.size() && (line[vbegin] == ' ' || line[vbegin] == '\t')) ++vbegin;
        const std::string value = line.substr(vbegin);

        if (name == "content-length") {
            char* endp = NULL;
            const long long v = std::strtoll(value.c_str(), &endp, 10);
            if (endp == value.c_str() || v < 0) {
                err = "Content-Length 非法: \"" + value + "\"";
                return false;
            }
            contentLength = v;
        } else if (name == "transfer-encoding") {
            if (LowerAscii(value).find("chunked") != std::string::npos) chunked = true;
        } else if (name == "connection") {
            const std::string lower = LowerAscii(value);
            if (lower.find("close") != std::string::npos) connClose = true;
            if (lower.find("keep-alive") != std::string::npos) connKeepAlive = true;
        }
    }
    return true;
}

} // namespace

/// 一条可跨轮复用的 HTTP/1.1 连接。
/// 同步阻塞语义: Exchange() 内部驱动私有 io_context 直到本轮结束; 因此只能在
/// 拉取工作线程使用。Cancel() 是唯一的跨线程入口, 它把关闭动作 post 进 io_context,
/// 让 Stop() 不必等满 timeoutMs。
/// 所有异步操作都以 shared_ptr 捕获 self, 绝不在 handler 中引用调用栈上的局部变量
/// —— handler 可能在发起它的栈帧返回之后才执行 (read 链尤其如此)。
class HttpExchange : public std::enable_shared_from_this<HttpExchange> {
public:
    HttpExchange()
        : _resolver(_io)
        , _socket(_io)
        , _timer(_io)
        , _connected(false)
        , _port(0)
        , _timeoutMs(0)
        , _wantKeepAlive(false)
        , _done(false)
        , _ok(false)
        , _answered(false)
        , _keepOpen(false)
        , _headParsed(false)
        , _status(0)
        , _contentLength(-1)
        , _chunked(false)
        , _chunkPhase(ChunkSize)
        , _chunkRemaining(0)
        , _chunkPos(0)
        , _http10(false)
        , _connClose(false)
        , _connKeepAlive(false)
        , _leftover(false)
        , _headEnd(std::string::npos)
        , _chunk(kReadChunkBytes)
        , _connectionsOpened(0) {
    }

    /// 一轮请求/响应。复用的 keep-alive 连接若已被对端静默关闭
    /// (一个响应字节都没收到), 自动新建连接重试一次。
    bool Exchange(const std::string& host, uint16_t port,
                  const std::string& request, int timeoutMs, bool keepAlive,
                  std::string& body, std::string& err) {
        const bool reuse = _connected && keepAlive
            && _host == host && _port == port;
        if (!reuse) Close();

        if (ExchangeOnce(host, port, request, timeoutMs, keepAlive, body, err)) {
            return true;
        }
        if (!reuse || _answered) return false;

        // 陈旧连接: 对端已下线, 但本轮还没读到任何字节, 换新连接重试.
        Close();
        body.clear();
        err.clear();
        return ExchangeOnce(host, port, request, timeoutMs, keepAlive, body, err);
    }

    /// 跨线程打断: post 到本连接的 io_context, 由工作线程自己关闭 socket。
    void Cancel() {
        std::shared_ptr<HttpExchange> self = shared_from_this();
        asio::post(_io, [self]() {
            asio::error_code ignored;
            self->_socket.close(ignored);
            self->_timer.cancel();
            self->_connected = false;
        });
    }

    /// 直接关闭 (仅在工作线程已 join 后调用, 否则用 Cancel)。
    void Shutdown() {
        Close();
        asio::error_code ignored;
        _timer.cancel(ignored);
    }

    bool Ok() const { return _ok; }
    const std::string& FailMessage() const { return _failMsg; }
    const std::string& Body() const { return _body; }
    /// Successful TCP establishments so far (keep-alive reuse keeps this flat).
    int ConnectionsOpened() const { return _connectionsOpened; }
    /// HTTP status of the last response (0 = none received).
    int Status() const { return _status; }

private:
    bool ExchangeOnce(const std::string& host, uint16_t port,
                      const std::string& request, int timeoutMs, bool keepAlive,
                      std::string& body, std::string& err) {
        _host = host;
        _port = port;
        _request = request;
        _timeoutMs = timeoutMs;
        _wantKeepAlive = keepAlive;
        ResetRound();

        std::shared_ptr<HttpExchange> self = shared_from_this();
        // run() 在上轮返回后会让 io_context 处于 stopped, 必须 restart 才能复用.
        _io.restart();
        _timer.expires_after(std::chrono::milliseconds(_timeoutMs));
        _timer.async_wait([self](const asio::error_code& ec) {
            if (!ec) {
                self->Finish(false, "请求超时 (" + std::to_string(self->_timeoutMs) + " ms)");
            }
        });

        if (_connected) {
            WriteRequest();
        } else {
            _resolver.async_resolve(host, std::to_string(port),
                [self](const asio::error_code& ec,
                       asio::ip::tcp::resolver::results_type results) {
                    if (ec) {
                        self->Finish(false, "DNS 解析失败: " + ec.message());
                        return;
                    }
                    asio::async_connect(self->_socket, results,
                        [self](const asio::error_code& cec,
                               const asio::ip::tcp::endpoint&) {
                            if (cec) {
                                self->Finish(false, "连接失败: " + cec.message());
                                return;
                            }
                            self->_connected = true;
                            ++self->_connectionsOpened;
                            self->WriteRequest();
                        });
                });
        }

        _io.run();
        if (!_ok) {
            err = _failMsg;
            Close();
            return false;
        }
        body = _body;
        return true;
    }

    void ResetRound() {
        _done = false;
        _ok = false;
        _failMsg.clear();
        _buffer.clear();
        _body.clear();
        _answered = false;
        _keepOpen = false;
        _headParsed = false;
        _status = 0;
        _contentLength = -1;
        _chunked = false;
        _chunkPhase = ChunkSize;
        _chunkRemaining = 0;
        _chunkPos = 0;
        _http10 = false;
        _connClose = false;
        _connKeepAlive = false;
        _leftover = false;
        _headEnd = std::string::npos;
    }

    void WriteRequest() {
        std::shared_ptr<HttpExchange> self = shared_from_this();
        asio::async_write(_socket, asio::buffer(_request),
            [self](const asio::error_code& wec, std::size_t) {
                if (wec) {
                    self->Finish(false, "发送请求失败: " + wec.message());
                    return;
                }
                self->ReadNext();
            });
    }

    void ReadNext() {
        std::shared_ptr<HttpExchange> self = shared_from_this();
        _socket.async_read_some(asio::buffer(_chunk),
            [self](const asio::error_code& rec, std::size_t n) {
                self->HandleRead(rec, n);
            });
    }

    void HandleRead(const asio::error_code& rec, std::size_t n) {
        if (_done) return;
        if (n > 0) {
            _buffer.append(&_chunk[0], n);
            _answered = true;
        }
        if (_buffer.size() > kMaxRawBufferBytes) {
            Finish(false, "响应超过 1MB 上限");
            return;
        }

        const bool eof = (rec == asio::error::eof);
        if (rec && !eof) {
            Finish(false, "读取响应失败: " + rec.message());
            return;
        }

        if (!_headParsed) {
            _headEnd = _buffer.find("\r\n\r\n");
            if (_headEnd != std::string::npos) {
                const std::string head = _buffer.substr(0, _headEnd);
                std::string perr;
                if (!ParseHead(head, _status, _contentLength, _chunked,
                               _http10, _connClose, _connKeepAlive, perr)) {
                    Finish(false, perr);
                    return;
                }
                if (_chunked) {
                    // chunked 定界: 从 body 起点开始按块解析 (见 ConsumeChunked).
                    _chunkPos = _headEnd + 4;
                    _chunkPhase = ChunkSize;
                }
                if (_status < 200 || _status >= 300) {
                    const std::string snippet = _buffer.substr(_headEnd + 4, 256);
                    Finish(false, "HTTP 状态 " + std::to_string(_status)
                        + (snippet.empty() ? std::string() : (": " + snippet)));
                    return;
                }
                if (_contentLength > static_cast<long long>(kMaxResponseBytes)) {
                    Finish(false, "Content-Length 超过 1MB 上限");
                    return;
                }
                _headParsed = true;
            }
        }

        if (_headParsed) {
            if (_chunked) {
                if (!ConsumeChunked()) return;      // 已结束 (成功或报错)
                if (eof) {
                    Finish(false, "chunked 响应提前结束");
                    return;
                }
                ReadNext();
                return;
            }
            const std::size_t have = _buffer.size() - (_headEnd + 4);
            if (_contentLength >= 0
                    && have >= static_cast<std::size_t>(_contentLength)) {
                _body = _buffer.substr(_headEnd + 4,
                                       static_cast<std::size_t>(_contentLength));
                // 多出来的字节说明响应边界不可信 (串包), 本轮结果照用但连接冲掉.
                _leftover = have > static_cast<std::size_t>(_contentLength);
                Finish(true, "");
                return;
            }
            if (_contentLength < 0 && eof) {
                _body = _buffer.substr(_headEnd + 4);
                Finish(true, "");
                return;
            }
        }

        if (eof) {
            Finish(false, _headParsed
                ? ("响应体提前结束 (期望 " + std::to_string(_contentLength) + " 字节)")
                : std::string("响应头不完整"));
            return;
        }
        ReadNext();
    }

    /// 消费缓冲区里已到达的 chunked 数据。
    /// 返回 false = 本轮已结束 (Finish 已被调用); true = 还需要更多字节。
    /// 只追加到 _body, 不解码 transfer-coding 之外的任何内容。
    bool ConsumeChunked() {
        for (;;) {
            if (_chunkPhase == ChunkSize) {
                const std::size_t eol = _buffer.find("\r\n", _chunkPos);
                if (eol == std::string::npos) {
                    if (_buffer.size() - _chunkPos > kMaxHeadBytes) {
                        Finish(false, "chunk 长度行过长");
                        return false;
                    }
                    return true;
                }
                std::string line = _buffer.substr(_chunkPos, eol - _chunkPos);
                const std::size_t semi = line.find(';');   // chunk extensions
                if (semi != std::string::npos) line = line.substr(0, semi);
                std::size_t begin = 0;
                std::size_t end = line.size();
                while (begin < end && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
                while (end > begin && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
                if (begin == end) {
                    Finish(false, "chunk 长度为空");
                    return false;
                }
                long long size = 0;
                for (std::size_t i = begin; i < end; ++i) {
                    const char c = line[i];
                    int digit = -1;
                    if (c >= '0' && c <= '9') digit = c - '0';
                    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
                    if (digit < 0) {
                        Finish(false, "chunk 长度非法: \"" + line + "\"");
                        return false;
                    }
                    size = size * 16 + digit;
                    if (size > static_cast<long long>(kMaxResponseBytes)) {
                        Finish(false, "chunk 长度超过 1MB 上限");
                        return false;
                    }
                }
                _chunkPos = eol + 2;
                if (size == 0) {
                    _chunkPhase = ChunkTrailer;
                    continue;
                }
                if (_body.size() + static_cast<std::size_t>(size) > kMaxResponseBytes) {
                    Finish(false, "响应超过 1MB 上限");
                    return false;
                }
                _chunkRemaining = size;
                _chunkPhase = ChunkData;
            }

            if (_chunkPhase == ChunkData) {
                const std::size_t avail = _buffer.size() - _chunkPos;
                const std::size_t need = static_cast<std::size_t>(_chunkRemaining);
                const std::size_t take = (avail < need) ? avail : need;
                if (take > 0) {
                    _body.append(_buffer, _chunkPos, take);
                    _chunkPos += take;
                    _chunkRemaining -= static_cast<long long>(take);
                }
                if (_chunkRemaining > 0) return true;
                _chunkPhase = ChunkDataCrLf;
            }

            if (_chunkPhase == ChunkDataCrLf) {
                if (_buffer.size() - _chunkPos < 2) return true;
                if (_buffer[_chunkPos] != '\r' || _buffer[_chunkPos + 1] != '\n') {
                    Finish(false, "chunk 数据后缺少 CRLF");
                    return false;
                }
                _chunkPos += 2;
                _chunkPhase = ChunkSize;
                continue;
            }

            // ChunkTrailer: 最后一个 0 长度块之后的 trailer 行, 空行结束。
            if (_buffer.size() - _chunkPos >= 2
                    && _buffer[_chunkPos] == '\r' && _buffer[_chunkPos + 1] == '\n') {
                _chunkPos += 2;
                // 响应之后还有字节 -> 边界不可信 (串包), 结果照用但连接冲掉。
                _leftover = _chunkPos < _buffer.size();
                Finish(true, "");
                return false;
            }
            const std::size_t eol = _buffer.find("\r\n", _chunkPos);
            if (eol == std::string::npos) {
                if (_buffer.size() - _chunkPos > kMaxHeadBytes) {
                    Finish(false, "chunk trailer 过长");
                    return false;
                }
                return true;
            }
            _chunkPos = eol + 2;   // trailer 内容忽略
        }
    }

    void Finish(bool success, const std::string& msg) {
        if (_done) return;
        _done = true;
        _ok = success;
        _failMsg = msg;

        // 复用条件: 本轮成功 + 客户端想复用 + 服务端未要求关闭
        // + 不是 "读到 EOF 才算结束" 的响应 + 没有串包字节.
        // chunked 由结束块定界, 与 Content-Length 同样可信。
        _keepOpen = success && _wantKeepAlive && !_connClose
            && !(_http10 && !_connKeepAlive)
            && (_chunked || _contentLength >= 0) && !_leftover;
        if (!_keepOpen) {
            asio::error_code ignored;
            _socket.close(ignored);
            _connected = false;
        }
        _timer.cancel();
    }

    void Close() {
        asio::error_code ignored;
        _socket.close(ignored);
        _connected = false;
    }

    asio::io_context _io;
    asio::ip::tcp::resolver _resolver;
    asio::ip::tcp::socket _socket;
    asio::steady_timer _timer;

    bool _connected;
    std::string _host;
    uint16_t _port;

    std::string _request;
    int _timeoutMs;
    bool _wantKeepAlive;

    bool _done;
    bool _ok;
    bool _answered;          // 本轮收到过响应字节 (用于陈旧连接判定)
    bool _keepOpen;
    std::string _failMsg;

    std::string _buffer;
    std::string _body;
    std::vector<char> _chunk;
    bool _headParsed;
    int _status;
    long long _contentLength;
    bool _chunked;
    enum ChunkPhase { ChunkSize, ChunkData, ChunkDataCrLf, ChunkTrailer };
    ChunkPhase _chunkPhase;
    long long _chunkRemaining;
    std::size_t _chunkPos;
    bool _http10;
    bool _connClose;
    bool _connKeepAlive;
    bool _leftover;
    std::size_t _headEnd;
    int _connectionsOpened;
};

HttpPuller::HttpPuller(asio::io_context& io,
                       const std::string& deviceId,
                       const Core::HttpPullConfig& cfg)
    : _io(io)
    , _deviceId(deviceId)
    , _cfg(cfg)
    , _reportedConnections(0)
    , _stop(false)
    , _running(false) {
    if (!ParseUrl(_cfg.url, _endpoint.host, _endpoint.port, _endpoint.path, _urlError)) {
        return;
    }
    _exchange = std::make_shared<HttpExchange>();
    BuildRequest();
}

HttpPuller::~HttpPuller() {
    Stop();
}

bool HttpPuller::ParseUrl(const std::string& url,
                          std::string& host, uint16_t& port,
                          std::string& path, std::string& err) {
    host.clear();
    path = "/";
    port = 80;
    if (url.compare(0, 7, "http://") != 0) {
        err = "URL 必须以 http:// 开头 (v1 不支持 https)";
        return false;
    }
    std::string rest = url.substr(7);
    if (rest.empty()) {
        err = "URL 缺少 host";
        return false;
    }
    if (rest.find('@') != std::string::npos) {
        err = "URL 不支持内嵌凭据, 请改用 headers";
        return false;
    }

    const std::size_t slash = rest.find_first_of("/?#");
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) {
        if (rest[slash] == '/') {
            path = rest.substr(slash);
        } else {
            path = "/" + rest.substr(slash);
        }
    }
    if (authority.empty()) {
        err = "URL 缺少 host";
        return false;
    }

    std::string portText;
    if (authority[0] == '[') {           // IPv6 literal: [::1]:8080
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) {
            err = "URL 的 IPv6 地址缺少 ]";
            return false;
        }
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') {
                err = "URL 的 IPv6 地址后存在非法字符";
                return false;
            }
            portText = authority.substr(close + 2);
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            portText = authority.substr(colon + 1);
        } else {
            host = authority;
        }
    }

    if (host.empty()) {
        err = "URL 缺少 host";
        return false;
    }
    for (std::size_t i = 0; i < host.size(); ++i) {
        const char c = host[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/') {
            err = "URL 的 host 含非法字符";
            return false;
        }
    }

    if (!portText.empty()) {
        char* endp = NULL;
        const long v = std::strtol(portText.c_str(), &endp, 10);
        if (endp == portText.c_str() || *endp != '\0' || v < 1 || v > 65535) {
            err = "URL 端口非法: \"" + portText + "\"";
            return false;
        }
        port = static_cast<uint16_t>(v);
    }
    if (path.empty() || path[0] != '/') path = "/" + path;
    return true;
}

void HttpPuller::BuildRequest() {
    BuildHttpRequest(_cfg, _endpoint.host, _endpoint.port, _endpoint.path, _request);
}

void HttpPuller::Start(FrameHandler onFrame, ErrorHandler onError) {
    _onFrame = onFrame;
    _onError = onError;

    if (!_urlError.empty()) {
        if (_onError) _onError(_urlError);
        _running.store(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = false;
    }
    _running.store(true);

    std::shared_ptr<HttpPuller> self = shared_from_this();
    _thread = std::thread([self]() { self->Loop(); });
}

void HttpPuller::Stop() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stop = true;
    }
    _cv.notify_all();
    // 打断正在阻塞的请求: 否则 join 要等满 timeoutMs.
    if (_exchange) _exchange->Cancel();
    if (_thread.joinable()) _thread.join();
    // 工作线程已退出: 把 keep-alive 连接真正关掉, 避免对端/测试服务端
    // 一直阻塞在 read 上 (Cancel 的 handler 可能在 io_context 停止后从未执行).
    if (_exchange) _exchange->Shutdown();
    _running.store(false);
}

int HttpPuller::ComputeRetryDelayMs(int failures, int intervalMs,
                                    int backoffBaseMs, int backoffMaxMs) {
    if (intervalMs < 1) intervalMs = 1;
    if (failures <= 0 || backoffBaseMs <= 0) return intervalMs;

    long long delay = backoffBaseMs;
    int steps = failures - 1;
    while (steps-- > 0 && delay < static_cast<long long>(backoffMaxMs)) {
        delay *= 2;
    }
    if (backoffMaxMs > 0 && delay > static_cast<long long>(backoffMaxMs)) {
        delay = backoffMaxMs;
    }
    if (delay < intervalMs) delay = intervalMs;
    if (delay > 0x7fffffffLL) delay = 0x7fffffffLL;
    return static_cast<int>(delay);
}

void HttpPuller::Loop() {
    int failures = 0;
    while (!StopRequested()) {
        Core::metrics::CounterInc(Core::metrics::kPullRoundsTotal,
                                  Core::metrics::Device(_deviceId));

        std::string body;
        std::string err;
        const bool ok = Exchange(_endpoint, _request, body, err);

        // 连接计数: keep-alive 命中时本值不增长, 可直接用来看复用率。
        const int opened = _exchange ? _exchange->ConnectionsOpened() : 0;
        if (opened > _reportedConnections) {
            Core::metrics::CounterInc(
                Core::metrics::kPullConnectionsTotal,
                Core::metrics::Device(_deviceId),
                static_cast<double>(opened - _reportedConnections));
            _reportedConnections = opened;
        }

        if (ok) {
            // Stop() 会主动打断在途请求, 此时的结果不再上报, 避免停机瞬间
            // 把标签刷成 Bad / 产生噪声告警.
            if (StopRequested()) break;
            if (failures > 0) {
                LOG_INFO("Southbound", "拉取恢复 device=%s, 连续失败 %d 次后恢复",
                         _deviceId.c_str(), failures);
                failures = 0;
            }
            Core::metrics::GaugeSet(Core::metrics::kPullConsecutiveFailures,
                                    Core::metrics::Device(_deviceId), 0);
            Core::metrics::GaugeSet(
                Core::metrics::kPullLastSuccessTimestamp,
                Core::metrics::Device(_deviceId),
                static_cast<double>(TagValueSink::NowMs() / 1000));
            PostHealth(true);
            PostFrame(body);
        } else {
            if (StopRequested()) break;
            ++failures;
            Core::metrics::CounterInc(Core::metrics::kPullFailuresTotal,
                                      Core::metrics::Device(_deviceId));
            Core::metrics::GaugeSet(Core::metrics::kPullConsecutiveFailures,
                                    Core::metrics::Device(_deviceId), failures);
            PostHealth(false);
            PostError(err);
        }

        const int delayMs = ComputeRetryDelayMs(failures, _cfg.intervalMs,
                                                _cfg.backoffBaseMs, _cfg.backoffMaxMs);
        Core::metrics::GaugeSet(Core::metrics::kPullBackoffMs,
                                Core::metrics::Device(_deviceId), delayMs);
        if (failures == 1 && delayMs > _cfg.intervalMs) {
            LOG_WARN("Southbound",
                     "拉取失败 device=%s, 进入指数退避 (基数 %d ms, 上限 %d ms)",
                     _deviceId.c_str(), _cfg.backoffBaseMs, _cfg.backoffMaxMs);
        }

        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait_for(lock, std::chrono::milliseconds(delayMs),
                     [this]() { return _stop; });
        if (_stop) break;
    }
    _running.store(false);
}

bool HttpPuller::StopRequested() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _stop;
}

void HttpPuller::PostFrame(const std::string& payload) {
    std::shared_ptr<HttpPuller> self = shared_from_this();
    InboundFrame frame;
    frame.topic.clear();
    frame.payload = payload;
    frame.timestampMs = TagValueSink::NowMs();
    asio::post(_io, [self, frame]() {
        if (self->_onFrame) self->_onFrame(frame);
    });
}

void HttpPuller::PostError(const std::string& message) {
    std::shared_ptr<HttpPuller> self = shared_from_this();
    asio::post(_io, [self, message]() {
        if (self->_onError) self->_onError(message);
    });
}

void HttpPuller::PostHealth(bool healthy) {
    if (!_onHealth) return;
    std::shared_ptr<HttpPuller> self = shared_from_this();
    const std::string deviceId = _deviceId;
    asio::post(_io, [self, deviceId, healthy]() {
        if (self->_onHealth) self->_onHealth(deviceId, healthy);
    });
}

bool HttpPuller::Exchange(const Endpoint& ep, const std::string& request,
                          std::string& body, std::string& err) {
    if (!_exchange) _exchange = std::make_shared<HttpExchange>();
    return _exchange->Exchange(ep.host, ep.port, request, _cfg.timeoutMs,
                               _cfg.keepAlive, body, err);
}

HttpPuller::ProbeResult HttpPuller::Probe(const Core::HttpPullConfig& cfg) {
    ProbeResult result;
    std::string host;
    std::string path;
    std::string err;
    uint16_t port = 0;
    if (!ParseUrl(cfg.url, host, port, path, err)) {
        result.error = err;
        return result;
    }

    // 试拉取是一次性请求: 报头必须与实际的 keepAlive=false 一致 (Connection: close),
    // 否则对端会把连接挂着等下一轮, 我们却已关闭。
    Core::HttpPullConfig probeCfg = cfg;
    probeCfg.keepAlive = false;
    std::string request;
    BuildHttpRequest(probeCfg, host, port, path, request);

    std::shared_ptr<HttpExchange> exchange = std::make_shared<HttpExchange>();
    std::string body;
    const std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    const bool ok = exchange->Exchange(host, port, request, probeCfg.timeoutMs,
                                       false, body, err);
    result.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count());
    result.status = exchange->Status();
    result.ok = ok;
    if (ok) result.body = body;
    else result.error = err;
    return result;
}

}} // namespace MyProt::Southbound

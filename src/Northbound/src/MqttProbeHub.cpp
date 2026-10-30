// src/Northbound/src/MqttProbeHub.cpp
// 观测会话实现 — 创建临时 MqttClient、缓存订阅事件、按轮次交付给 HTTP/SSE 层

#include "MyProt/Northbound/MqttProbeHub.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <vector>

#include <nlohmann/json.hpp>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Core/MqttConfig.hpp"
#include "MyProt/Core/Platform.hpp"
#include "MyProt/Northbound/MqttClient.hpp"

namespace MyProt { namespace Northbound {

namespace {

/// 墙钟毫秒 — 事件时间戳 (前端按本地时间格式化)
int64_t EpochMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

/// 单调毫秒 — 心跳 / 空闲回收的时间基准 (不受系统时钟调整影响)
int64_t SteadyMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string Trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

/// 纯数字端口的严格解析; 非法返回 -1
int ParsePort(const std::string& text) {
    if (text.empty() || text.size() > 5) return -1;
    int value = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

std::string HexEncode(const char* data, std::size_t size) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        out.push_back(kHex[(c >> 4) & 0x0F]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

/// 序列化为可下发文本
/// JSON 必须是 UTF-8, 而 lastError 可能来自 asio/系统 (中文 Windows 下 ec.message()
/// 是本地代码页 GBK), broker 下发的主题也可能是二进制 —— 默认 strict 会抛
/// type_error.316, 令整个帧 500 / SSE 断流。用 replace 把非法序列换成 U+FFFD。
std::string DumpFrame(const nlohmann::json& doc) {
    return doc.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

/// 把系统错误文本转成可下发的 UTF-8
/// asio/系统错误消息跟随本地代码页 (中文 Windows 上是 GBK), 直接下发前端只会显示乱码;
/// 已是合法 UTF-8 的原样返回, 否则在 Windows 上按 ACP 转 UTF-8, 其它平台交给 DumpFrame 兜底。
std::string ToDisplayText(const std::string& text) {
    if (text.empty() || MqttProbeHub::IsValidUtf8(text)) return text;
#if defined(_WIN32)
    const int need = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (need <= 0) return text;
    std::wstring wide(static_cast<size_t>(need) - 1, L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], need);
    const std::string utf8 = Core::Platform::FromWide(wide);
    return utf8.empty() ? text : utf8;
#else
    return text;
#endif
}

nlohmann::json StatusEvent(const std::string& state, const std::string& detail) {
    nlohmann::json ev;
    ev["type"] = "status";
    ev["state"] = state;
    ev["detail"] = detail;
    return ev;
}

/// 会话 id — 时间与自增计数混合后取 16 位十六进制, 进程内唯一即可
std::string MakeSid() {
    static std::atomic<unsigned long long> counter(0);
    unsigned long long mixed =
        static_cast<unsigned long long>(EpochMs()) * 1000003ULL;
    mixed ^= (++counter) * 0x9E3779B97F4A7C15ULL;
    mixed ^= mixed >> 29;

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 16; ++i) {
        out.push_back(kHex[mixed & 0x0F]);
        mixed = mixed * 6364136223846793005ULL + 1442695040888963407ULL;
    }
    return out;
}

bool HasSchemePrefix(const std::string& text, const char* prefix) {
    const std::size_t len = std::string(prefix).size();
    if (text.size() < len) return false;
    for (std::size_t i = 0; i < len; ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i]))
                != static_cast<unsigned char>(prefix[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

struct MqttProbeHub::Session {
    std::string sid;
    std::string topic;
    int qos;
    std::shared_ptr<MqttClient> client;
    std::deque<nlohmann::json> events;
    std::string state;        // connecting | online | error
    std::string lastError;
    uint64_t received;
    uint64_t dropped;
    int64_t lastPollMs;       // 最近一次 SSE 取走事件的时间 (空闲回收依据)
    int64_t lastEmitMs;       // 最近一次送出帧的时间 (心跳依据)

    Session() : qos(0), received(0), dropped(0),
                lastPollMs(0), lastEmitMs(0) {}
};

MqttProbeHub::MqttProbeHub(const Options& opts)
    : _opts(opts), _running(false) {
    if (_opts.maxSessions <= 0) _opts.maxSessions = 8;
    if (_opts.idleTimeoutMs < 1000) _opts.idleTimeoutMs = 1000;
    if (_opts.statusTickMs < 50) _opts.statusTickMs = 50;
    if (_opts.heartbeatMs < 500) _opts.heartbeatMs = 500;
    if (_opts.maxEvents == 0) _opts.maxEvents = 500;
    if (_opts.maxPayloadBytes == 0) _opts.maxPayloadBytes = 8192;

    _io.reset(new asio::io_context());
    _work.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        _io->get_executor()));
    _tick.reset(new asio::steady_timer(*_io));

    _running.store(true);
    _thread = std::thread([this]() {
        try {
            _io->run();
        } catch (const std::exception& e) {
            LOG_ERROR("MQTT", "观测会话 IO 线程异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("MQTT", "观测会话 IO 线程未知异常");
        }
    });
    asio::post(*_io, [this]() { ArmTick(); });
}

MqttProbeHub::~MqttProbeHub() {
    StopAll();
    _running.store(false);
    if (_tick) {
        asio::error_code ignore;
        _tick->cancel(ignore);
    }
    if (_io) _io->stop();
    if (_thread.joinable()) _thread.join();
    _work.reset();
    _tick.reset();
    _io.reset();
}

// ──────────────────────────── 校验工具 ────────────────────────────

bool MqttProbeHub::NormalizeBroker(std::string* host, int* port) {
    if (!host || !port) return false;

    std::string text = Trim(*host);
    const char* kSchemes[4] = { "mqtt://", "tcp://", "mqtts://", "ssl://" };
    for (std::size_t i = 0; i < 4; ++i) {
        if (HasSchemePrefix(text, kSchemes[i])) {
            text = text.substr(std::string(kSchemes[i]).size());
            break;
        }
    }
    // 丢弃 URL 尾部的路径/查询串
    const std::size_t slash = text.find('/');
    if (slash != std::string::npos) text = text.substr(0, slash);

    std::string bareHost;
    int parsedPort = 0;
    if (!text.empty() && text[0] == '[') {          // IPv6 字面量 [::1]:1883
        const std::size_t close = text.find(']');
        if (close == std::string::npos) return false;
        bareHost = text.substr(1, close - 1);
        if (close + 1 < text.size()) {
            if (text[close + 1] != ':') return false;
            parsedPort = ParsePort(text.substr(close + 2));
            if (parsedPort < 0) return false;
        }
    } else {
        const std::size_t colon = text.rfind(':');
        if (colon != std::string::npos && text.find(':') == colon) {
            parsedPort = ParsePort(text.substr(colon + 1));
            if (parsedPort < 0) return false;
            bareHost = text.substr(0, colon);
        } else {
            bareHost = text;
        }
    }

    bareHost = Trim(bareHost);
    if (bareHost.empty() || bareHost.size() > 253) return false;
    for (std::size_t i = 0; i < bareHost.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bareHost[i]);
        if (std::isspace(c) || c == '/' || c == '?' || c == '#') return false;
    }

    // host 中显式写出的端口优先于独立字段
    if (parsedPort > 0) *port = parsedPort;
    if (*port == 0) *port = 1883;
    if (*port < 1 || *port > 65535) return false;

    *host = bareHost;
    return true;
}

bool MqttProbeHub::IsValidTopicFilter(const std::string& filter) {
    if (filter.empty() || filter.size() > 512) return false;

    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = filter.find('/', start);
        const bool last = (slash == std::string::npos);
        const std::string level = last
            ? filter.substr(start) : filter.substr(start, slash - start);

        for (std::size_t i = 0; i < level.size(); ++i) {
            if (level[i] == '#') {
                // '#' 必须独占最后一层
                if (!last || level != "#") return false;
            } else if (level[i] == '+') {
                // '+' 必须独占一层
                if (level != "+") return false;
            }
        }

        if (last) break;
        start = slash + 1;
    }
    return true;
}

bool MqttProbeHub::IsValidUtf8(const std::string& text) {
    const std::size_t size = text.size();
    std::size_t i = 0;
    while (i < size) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80) { ++i; continue; }

        std::size_t extra = 0;
        unsigned int cp = 0;
        if ((lead & 0xE0) == 0xC0) {
            extra = 1;
            cp = lead & 0x1F;
            if (cp < 2) return false;                 // overlong
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
            cp = lead & 0x0F;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
            cp = lead & 0x07;
        } else {
            return false;                             // 0x80..0xBF / 0xF8..0xFF
        }
        if (i + extra >= size) return false;

        for (std::size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;   // surrogate

        i += extra + 1;
    }
    return true;
}

// ──────────────────────────── 会话生命周期 ────────────────────────────

std::string MqttProbeHub::Create(const MqttProbeSpec& spec, std::string* error) {
    std::string localError;
    std::string* err = error ? error : &localError;
    err->clear();

    if (!_running.load()) {
        *err = "观测服务已停止";
        return std::string();
    }

    MqttProbeSpec normalized = spec;
    if (!NormalizeBroker(&normalized.host, &normalized.port)) {
        *err = "broker 地址非法 (可用 host、host:port 或 mqtt://host:port)";
        return std::string();
    }
    if (!IsValidTopicFilter(normalized.topic)) {
        *err = "订阅过滤器非法 (仅支持 # 结尾通配与完整层级 +)";
        return std::string();
    }
    if (normalized.qos < 0 || normalized.qos > 1) {
        *err = "仅支持 QoS 0/1";
        return std::string();
    }
    if (!normalized.clientId.empty() && normalized.clientId.size() > 64) {
        *err = "clientId 过长 (最多 64 字节)";
        return std::string();
    }
    if (normalized.keepAliveSec > 3600) {
        *err = "心跳须在 0..3600 秒";
        return std::string();
    }
    if (normalized.keepAliveSec <= 0) normalized.keepAliveSec = 60;

    std::shared_ptr<Session> session(new Session());
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (static_cast<int>(_sessions.size()) >= _opts.maxSessions) {
            *err = "同时最多 " + std::to_string(_opts.maxSessions) + " 个观测会话";
            return std::string();
        }
        std::string sid = MakeSid();
        while (_sessions.find(sid) != _sessions.end()) sid = MakeSid();

        session->sid = sid;
        session->topic = normalized.topic;
        session->qos = normalized.qos;
        session->state = "connecting";
        session->events.push_back(StatusEvent("connecting", std::string()));
        session->lastPollMs = SteadyMs();
        session->lastEmitMs = 0;
        _sessions[sid] = session;

        const std::string postedSid = sid;
        asio::post(*_io, [this, postedSid, normalized]() {
            StartSession(postedSid, normalized);
        });
        return sid;
    }
}

void MqttProbeHub::StartSession(const std::string& sid,
                                const MqttProbeSpec& spec) {
    if (!_running.load()) return;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_sessions.find(sid) == _sessions.end()) return;   // 已被 Stop
    }

    Core::MqttConfig cfg;
    cfg.enabled = true;
    cfg.host = spec.host;
    cfg.port = spec.port;
    cfg.clientId = spec.clientId.empty()
        ? ("myprot-probe-" + sid.substr(0, 8)) : spec.clientId;
    cfg.username = spec.username;
    cfg.password = spec.password;
    cfg.keepAliveSec = spec.keepAliveSec > 0 ? spec.keepAliveSec : 60;
    cfg.reconnectMinMs = 1000;
    cfg.reconnectMaxMs = 10000;

    std::shared_ptr<MqttClient> client(new MqttClient(*_io, cfg));
    client->SetCleanSession(true);
    client->SetOnMessageEx(
        [this, sid](const std::string& topic, const std::string& payload,
                    int qos, bool retain) {
            OnMessage(sid, topic, payload, qos, retain);
        });
    client->SetOnConnected([this, sid]() {
        std::lock_guard<std::mutex> lock(_mutex);
        std::map<std::string, std::shared_ptr<Session> >::iterator it =
            _sessions.find(sid);
        if (it == _sessions.end()) return;
        Session& s = *it->second;
        s.state = "online";
        s.lastError.clear();
        s.events.push_back(StatusEvent("online", std::string()));
        if (s.events.size() > _opts.maxEvents) s.events.pop_front();
    });
    client->Subscribe(spec.topic, spec.qos);

    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::map<std::string, std::shared_ptr<Session> >::iterator it =
            _sessions.find(sid);
        if (it == _sessions.end()) {
            client->Stop();
            return;
        }
        it->second->client = client;
    }
    client->Start();

    LOG_INFO("MQTT", "观测会话已启动 sid=%s broker=%s:%d topic=%s qos=%d",
             sid.c_str(), spec.host.c_str(), spec.port,
             spec.topic.c_str(), spec.qos);
}

void MqttProbeHub::OnMessage(const std::string& sid,
                             const std::string& topic,
                             const std::string& payload,
                             int qos, bool retain) {
    nlohmann::json ev;
    ev["type"] = "message";
    ev["topic"] = topic;
    ev["qos"] = qos;
    ev["retain"] = retain;
    ev["ts"] = EpochMs();
    ev["bytes"] = static_cast<uint64_t>(payload.size());

    const std::size_t cap = _opts.maxPayloadBytes;
    if (payload.size() > cap) {
        // 超长载荷只保留前缀; 前缀按 hex 交付, 避免截断在多字节字符中间
        ev["encoding"] = "hex";
        ev["payload"] = HexEncode(payload.data(), cap);
        ev["truncated"] = true;
    } else if (IsValidUtf8(payload)) {
        ev["encoding"] = "utf8";
        ev["payload"] = payload;
        ev["truncated"] = false;
    } else {
        ev["encoding"] = "hex";
        ev["payload"] = HexEncode(payload.data(), payload.size());
        ev["truncated"] = false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, std::shared_ptr<Session> >::iterator it =
        _sessions.find(sid);
    if (it == _sessions.end()) return;
    Session& s = *it->second;
    ++s.received;
    if (s.events.size() >= _opts.maxEvents) {
        s.events.pop_front();
        ++s.dropped;
    }
    s.events.push_back(ev);
}

std::string MqttProbeHub::Poll(const std::string& sid) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, std::shared_ptr<Session> >::iterator it =
        _sessions.find(sid);
    if (it == _sessions.end()) {
        nlohmann::json closed;
        closed["state"] = "closed";
        closed["connected"] = false;
        closed["received"] = 0;
        closed["dropped"] = 0;
        closed["lastError"] = "session not found";
        closed["events"] = nlohmann::json::array();
        return DumpFrame(closed);
    }

    Session& s = *it->second;
    const int64_t now = SteadyMs();
    s.lastPollMs = now;

    if (s.events.empty() && (now - s.lastEmitMs) < _opts.heartbeatMs) {
        return std::string();
    }

    nlohmann::json events = nlohmann::json::array();
    while (!s.events.empty()) {
        events.push_back(s.events.front());
        s.events.pop_front();
    }

    nlohmann::json out;
    out["state"] = s.state;
    out["connected"] = (s.state == "online");
    out["received"] = s.received;
    out["dropped"] = s.dropped;
    out["lastError"] = s.lastError;
    out["events"] = events;
    s.lastEmitMs = now;
    return DumpFrame(out);
}

std::string MqttProbeHub::Status(const std::string& sid) {
    std::lock_guard<std::mutex> lock(_mutex);
    std::map<std::string, std::shared_ptr<Session> >::iterator it =
        _sessions.find(sid);
    if (it == _sessions.end()) return std::string();

    Session& s = *it->second;
    nlohmann::json out;
    out["sid"] = s.sid;
    out["topic"] = s.topic;
    out["qos"] = s.qos;
    out["state"] = s.state;
    out["connected"] = (s.state == "online");
    out["received"] = s.received;
    out["dropped"] = s.dropped;
    out["queued"] = static_cast<uint64_t>(s.events.size());
    out["lastError"] = s.lastError;
    return DumpFrame(out);
}

bool MqttProbeHub::Publish(const std::string& sid, const std::string& topic,
                           const std::string& payload, int qos, bool retain,
                           std::string* error) {
    std::string localError;
    std::string* err = error ? error : &localError;

    if (topic.empty() || topic.size() > 512
            || topic.find('#') != std::string::npos
            || topic.find('+') != std::string::npos) {
        *err = "发布主题非法 (不允许通配符)";
        return false;
    }
    if (qos < 0 || qos > 1) {
        *err = "仅支持 QoS 0/1";
        return false;
    }

    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::map<std::string, std::shared_ptr<Session> >::iterator it =
            _sessions.find(sid);
        if (it == _sessions.end()) {
            *err = "会话不存在";
            return false;
        }
        session = it->second;
    }
    if (!session->client || !session->client->IsConnected()) {
        *err = "连接未建立";
        return false;
    }
    if (!session->client->Publish(topic, payload, qos, retain)) {
        *err = "发送失败";
        return false;
    }
    return true;
}

bool MqttProbeHub::Stop(const std::string& sid) {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_sessions.find(sid) == _sessions.end()) return false;
    }
    RemoveSession(sid);
    return true;
}

void MqttProbeHub::StopAll() {
    std::vector<std::string> sids;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        for (std::map<std::string, std::shared_ptr<Session> >::const_iterator it =
                 _sessions.begin(); it != _sessions.end(); ++it) {
            sids.push_back(it->first);
        }
    }
    for (std::size_t i = 0; i < sids.size(); ++i) RemoveSession(sids[i]);
}

void MqttProbeHub::RemoveSession(const std::string& sid) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::map<std::string, std::shared_ptr<Session> >::iterator it =
            _sessions.find(sid);
        if (it == _sessions.end()) return;
        session = it->second;
        _sessions.erase(it);
    }
    if (session && session->client) session->client->Stop();
    LOG_INFO("MQTT", "观测会话已停止 sid=%s", sid.c_str());
}

std::size_t MqttProbeHub::Count() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _sessions.size();
}

// ──────────────────────────── 状态采样 / 空闲回收 ────────────────────────────

void MqttProbeHub::ArmTick() {
    if (!_running.load() || !_tick) return;
    _tick->expires_after(std::chrono::milliseconds(_opts.statusTickMs));
    _tick->async_wait([this](const asio::error_code& ec) {
        if (ec || !_running.load()) return;
        OnTick();
        ArmTick();
    });
}

void MqttProbeHub::OnTick() {
    if (!_running.load()) return;

    std::vector<std::shared_ptr<Session> > expired;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        const int64_t now = SteadyMs();
        for (std::map<std::string, std::shared_ptr<Session> >::iterator it =
                 _sessions.begin(); it != _sessions.end(); ++it) {
            Session& s = *it->second;
            const bool connected = s.client && s.client->IsConnected();
            const std::string err = s.client
                ? ToDisplayText(s.client->LastError()) : std::string();
            const std::string next = connected
                ? "online" : (err.empty() ? "connecting" : "error");

            if (next != s.state) {
                s.state = next;
                s.lastError = connected ? std::string() : err;
                if (s.events.size() >= _opts.maxEvents) {
                    s.events.pop_front();
                    ++s.dropped;
                }
                s.events.push_back(StatusEvent(next, s.lastError));
            } else if (!connected && err != s.lastError) {
                s.lastError = err;
                if (s.events.size() >= _opts.maxEvents) {
                    s.events.pop_front();
                    ++s.dropped;
                }
                s.events.push_back(StatusEvent(next, err));
            }

            if (now - s.lastPollMs > _opts.idleTimeoutMs) {
                expired.push_back(it->second);
            }
        }

        for (std::size_t i = 0; i < expired.size(); ++i) {
            _sessions.erase(expired[i]->sid);
        }
    }

    // 锁外停止客户端: MqttClient::Stop 内部 post 到本线程, 持有 shared_ptr 保活
    for (std::size_t i = 0; i < expired.size(); ++i) {
        if (expired[i]->client) expired[i]->client->Stop();
        LOG_INFO("MQTT", "观测会话空闲回收 sid=%s", expired[i]->sid.c_str());
    }
}

}} // namespace MyProt::Northbound

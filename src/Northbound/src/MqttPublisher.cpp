// src/Northbound/src/MqttPublisher.cpp
// 北向 MQTT 发布器实现 — 定时取快照 → 变化过滤 → topic/payload 组帧 → 客户端发送

#include "MyProt/Northbound/MqttPublisher.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <nlohmann/json.hpp>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Northbound/MqttClient.hpp"

namespace MyProt { namespace Northbound {

namespace {

int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

const char* TypeName(Core::ValueType t) {
    switch (t) {
    case Core::ValueType::ByteArray: return "bytes";
    case Core::ValueType::UInt16:    return "uint16";
    case Core::ValueType::Int16:     return "int16";
    case Core::ValueType::UInt32:    return "uint32";
    case Core::ValueType::Int32:     return "int32";
    case Core::ValueType::UInt64:    return "uint64";
    case Core::ValueType::Int64:     return "int64";
    case Core::ValueType::Float:     return "float";
    case Core::ValueType::Double:    return "double";
    case Core::ValueType::Bool:      return "bool";
    case Core::ValueType::String:    return "string";
    default:                         return "empty";
    }
}

const char* QualityName(Core::QualityCode q) {
    switch (q) {
    case Core::QualityCode::Good:      return "Good";
    case Core::QualityCode::Uncertain: return "Uncertain";
    default:                           return "Bad";
    }
}

std::string HexEncode(const std::vector<uint8_t>& bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out.push_back(kHex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

/// 有限双精度转字符串 (%.17g 保证可逆); NaN/Inf 返回空串
std::string DoubleText(double d) {
    if (!std::isfinite(d)) return std::string();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", d);
    return std::string(buf);
}

/// MQTT topic 层级分隔符与通配符不允许出现在单层名称中
std::string SanitizeLevel(const std::string& s) {
    std::string out = s;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == '/' || c == '+' || c == '#') out[i] = '_';
    }
    return out.empty() ? std::string("_") : out;
}

} // namespace

MqttPublisher::MqttPublisher(Polling::LatestValueStore& store)
    : _store(store)
    , _cfg()
    , _running(false)
    , _published(0) {}

MqttPublisher::~MqttPublisher() {
    Stop();
}

void MqttPublisher::Apply(const Core::MqttConfig& cfg) {
    if (!cfg.enabled) {
        if (_running.load()) Stop();
        _cfg = cfg;          // 保留字段, 便于下次启用时直接比对
        return;
    }
    if (_running.load() && SameConfig(cfg, _cfg)) return;
    Stop();
    Start(cfg);
}

void MqttPublisher::Stop() {
    _running.store(false);
    if (_client) _client->Stop();
    if (_io) _io->stop();
    if (_thread.joinable()) _thread.join();
    _client.reset();
    _work.reset();
    _tickTimer.reset();
    _io.reset();
    _publishedSignatures.clear();
}

void MqttPublisher::Start(const Core::MqttConfig& cfg) {
    _cfg = cfg;
    _publishedSignatures.clear();

    _io.reset(new asio::io_context());
    _work.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        _io->get_executor()));
    _tickTimer.reset(new asio::steady_timer(*_io));
    _client.reset(new MqttClient(*_io, cfg));

    // 在线状态 + 遗嘱: 进程异常退出时 broker 代发 offline
    const std::string statusTopic = cfg.topicPrefix + "/status";
    _client->SetWill(statusTopic, "{\"status\":\"offline\"}", 0, true);
    _client->SetOnConnected([this]() {
        // 重连后缓存失效 → "changed" 模式全量补发一次当前值
        _publishedSignatures.clear();
        PublishStatus("online");
    });

    _running.store(true);
    _thread = std::thread([this]() {
        try {
            _io->run();
        } catch (const std::exception& e) {
            LOG_ERROR("MQTT", "IO 线程异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("MQTT", "IO 线程未知异常");
        }
    });

    _client->Start();
    asio::post(*_io, [this]() { ScheduleTick(); });

    LOG_INFO("MQTT",
             "北向发布已启动: broker=%s:%d prefix=%s mode=%s interval=%dms qos=%d retain=%d",
             cfg.host.c_str(), cfg.port, cfg.topicPrefix.c_str(),
             cfg.publishMode.c_str(), cfg.publishIntervalMs, cfg.qos,
             cfg.retain ? 1 : 0);
}

void MqttPublisher::ScheduleTick() {
    if (!_running.load() || !_tickTimer) return;
    _tickTimer->expires_after(std::chrono::milliseconds(_cfg.publishIntervalMs));
    _tickTimer->async_wait([this](const asio::error_code& ec) {
        if (ec || !_running.load()) return;
        OnTick();
        ScheduleTick();
    });
}

void MqttPublisher::OnTick() {
    if (!_client || !_client->IsConnected()) return;   // 断线期间不累积, 重连后全量补发

    const std::vector<Core::TagValue> values = _store.Snapshot();
    for (std::size_t i = 0; i < values.size(); ++i) {
        const Core::TagValue& tv = values[i];
        const std::string signature = BuildSignature(tv);

        std::map<std::string, std::string>::const_iterator it =
            _publishedSignatures.find(tv.tagName);
        if (_cfg.publishMode == "changed" && it != _publishedSignatures.end()
                && it->second == signature) {
            continue;
        }

        if (!_client->Publish(BuildTopic(tv), BuildPayload(tv),
                              _cfg.qos, _cfg.retain)) {
            // 队列刚发现断连: 清缓存, 让重连后的首轮全量重发
            _publishedSignatures.clear();
            return;
        }
        _publishedSignatures[tv.tagName] = signature;
        ++_published;
    }
}

void MqttPublisher::PublishStatus(const char* status) {
    if (!_client) return;
    nlohmann::json j;
    j["status"] = status;
    j["clientId"] = _cfg.clientId;
    j["ts"] = NowMs();
    _client->Publish(_cfg.topicPrefix + "/status", j.dump(), 0, true);
}

std::string MqttPublisher::BuildTopic(const Core::TagValue& tv) const {
    return _cfg.topicPrefix + "/" + SanitizeLevel(tv.deviceId) + "/"
           + SanitizeLevel(tv.tagName);
}

std::string MqttPublisher::BuildPayload(const Core::TagValue& tv) const {
    nlohmann::json j;
    j["device"] = tv.deviceId;
    j["tag"] = tv.tagName;
    j["type"] = TypeName(tv.typedValue.type);

    const Core::TypedValue& v = tv.typedValue;
    switch (v.type) {
    case Core::ValueType::ByteArray:
        j["value"] = HexEncode(v.bytes);
        break;
    case Core::ValueType::UInt16:
    case Core::ValueType::UInt32:
    case Core::ValueType::UInt64:
        j["value"] = v.u;
        break;
    case Core::ValueType::Int16:
    case Core::ValueType::Int32:
    case Core::ValueType::Int64:
        j["value"] = v.i;
        break;
    case Core::ValueType::Float:
    case Core::ValueType::Double:
        if (std::isfinite(v.d)) {
            j["value"] = v.d;
        } else {
            j["value"] = nullptr;   // JSON 不支持 NaN/Inf
        }
        break;
    case Core::ValueType::Bool:
        j["value"] = v.b;
        break;
    case Core::ValueType::String:
        j["value"] = v.str;
        break;
    default:
        j["value"] = nullptr;
        break;
    }

    j["quality"] = QualityName(tv.quality);
    j["ts"] = tv.timestamp;
    j["requestId"] = tv.requestId;
    j["changed"] = tv.valueChanged;
    return j.dump();
}

std::string MqttPublisher::BuildSignature(const Core::TagValue& tv) {
    const Core::TypedValue& v = tv.typedValue;
    std::string s = std::string(TypeName(v.type)) + "|" + QualityName(tv.quality) + "|";
    switch (v.type) {
    case Core::ValueType::ByteArray:
        s += HexEncode(v.bytes);
        break;
    case Core::ValueType::UInt16:
    case Core::ValueType::UInt32:
    case Core::ValueType::UInt64:
        s += std::to_string(static_cast<unsigned long long>(v.u));
        break;
    case Core::ValueType::Int16:
    case Core::ValueType::Int32:
    case Core::ValueType::Int64:
        s += std::to_string(static_cast<long long>(v.i));
        break;
    case Core::ValueType::Float:
    case Core::ValueType::Double:
        s += DoubleText(v.d);
        break;
    case Core::ValueType::Bool:
        s += v.b ? "1" : "0";
        break;
    case Core::ValueType::String:
        s += v.str;
        break;
    default:
        break;
    }
    return s;
}

bool MqttPublisher::SameConfig(const Core::MqttConfig& a,
                               const Core::MqttConfig& b) {
    return a.enabled == b.enabled
        && a.host == b.host
        && a.port == b.port
        && a.clientId == b.clientId
        && a.username == b.username
        && a.password == b.password
        && a.keepAliveSec == b.keepAliveSec
        && a.topicPrefix == b.topicPrefix
        && a.publishMode == b.publishMode
        && a.publishIntervalMs == b.publishIntervalMs
        && a.qos == b.qos
        && a.retain == b.retain;
}

}} // namespace MyProt::Northbound

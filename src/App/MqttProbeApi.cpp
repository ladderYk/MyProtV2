// src/App/MqttProbeApi.cpp — /api/mqtt/probe/* 与 /api/mqtt/stream 的 HTTP 适配
// 职责收敛: 本文件只做 body 解析 / 状态码映射, 观测会话本体在 Northbound::MqttProbeHub。

#include "MqttProbeApi.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "MyProt/Northbound/MqttProbeHub.hpp"
#include "RuntimeGlue.hpp"

namespace MyProt { namespace App {

namespace {

typedef std::pair<int, std::string> Resp;

Resp JsonResponse(int status, const std::string& error) {
    nlohmann::json doc;
    doc["error"] = error;
    // 错误文本可能是源码字面量或系统消息, 未必是合法 UTF-8;
    // strict 会抛 type_error 把 400 变 500, 故用 replace (非法序列 → U+FFFD)
    return Resp(status, doc.dump(-1, ' ', false,
                                nlohmann::json::error_handler_t::replace));
}

/// 按 '/' 切分路径, 忽略空段 ("/api//x" 与 "/api/x" 等价)
std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > start) out.push_back(path.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return out;
}

bool ReadString(const nlohmann::json& doc, const char* key,
                std::string* out, bool* present, std::string* error) {
    nlohmann::json::const_iterator it = doc.find(key);
    if (it == doc.end()) {
        *present = false;
        return true;
    }
    if (!it->is_string()) {
        *error = std::string(key) + " 须为字符串";
        return false;
    }
    *out = it->get<std::string>();
    *present = true;
    return true;
}

bool ReadInt(const nlohmann::json& doc, const char* key,
             int* out, bool* present, std::string* error) {
    nlohmann::json::const_iterator it = doc.find(key);
    if (it == doc.end()) {
        *present = false;
        return true;
    }
    if (!it->is_number_integer()) {
        *error = std::string(key) + " 须为整数";
        return false;
    }
    *out = it->get<int>();
    *present = true;
    return true;
}

bool ReadBool(const nlohmann::json& doc, const char* key,
              bool* out, bool* present, std::string* error) {
    nlohmann::json::const_iterator it = doc.find(key);
    if (it == doc.end()) {
        *present = false;
        return true;
    }
    if (!it->is_boolean()) {
        *error = std::string(key) + " 须为 bool";
        return false;
    }
    *out = it->get<bool>();
    *present = true;
    return true;
}

/// POST /api/mqtt/probe — 建立观测会话
Resp CreateSession(Northbound::MqttProbeHub& hub, const std::string& body) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return JsonResponse(400, "JSON 解析失败");
    } catch (const std::exception&) {
        return JsonResponse(400, "内部错误");
    }
    if (!doc.is_object()) return JsonResponse(400, "body 须为 JSON 对象");

    Northbound::MqttProbeSpec spec;
    std::string error;
    bool present = false;

    if (!ReadString(doc, "host", &spec.host, &present, &error))
        return JsonResponse(400, error);
    if (!ReadInt(doc, "port", &spec.port, &present, &error))
        return JsonResponse(400, error);
    if (!ReadString(doc, "topic", &spec.topic, &present, &error))
        return JsonResponse(400, error);
    if (!ReadInt(doc, "qos", &spec.qos, &present, &error))
        return JsonResponse(400, error);
    if (!ReadString(doc, "clientId", &spec.clientId, &present, &error))
        return JsonResponse(400, error);
    if (!ReadString(doc, "username", &spec.username, &present, &error))
        return JsonResponse(400, error);
    if (!ReadString(doc, "password", &spec.password, &present, &error))
        return JsonResponse(400, error);
    if (!ReadInt(doc, "keepAliveSec", &spec.keepAliveSec, &present, &error))
        return JsonResponse(400, error);

    const std::string sid = hub.Create(spec, &error);
    if (sid.empty()) {
        // 容量上限是"暂时不可用", 其余是入参错误
        const int status = (error.find("同时最多") != std::string::npos) ? 429 : 400;
        return JsonResponse(status, error);
    }

    nlohmann::json out;
    out["sid"] = sid;
    out["stream"] = "/api/mqtt/stream?sid=" + sid;
    return Resp(200, out.dump());
}

/// POST /api/mqtt/probe/{sid}/publish — 借会话连接发布
Resp PublishToSession(Northbound::MqttProbeHub& hub, const std::string& sid,
                      const std::string& body) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return JsonResponse(400, "JSON 解析失败");
    } catch (const std::exception&) {
        return JsonResponse(400, "内部错误");
    }
    if (!doc.is_object()) return JsonResponse(400, "body 须为 JSON 对象");

    std::string topic;
    std::string payload;
    int qos = 0;
    bool retain = false;
    std::string error;
    bool present = false;

    if (!ReadString(doc, "topic", &topic, &present, &error))
        return JsonResponse(400, error);
    if (!present) return JsonResponse(400, "topic 必填");
    if (!ReadString(doc, "payload", &payload, &present, &error))
        return JsonResponse(400, error);
    if (!present) return JsonResponse(400, "payload 必填");
    if (!ReadInt(doc, "qos", &qos, &present, &error))
        return JsonResponse(400, error);
    if (!ReadBool(doc, "retain", &retain, &present, &error))
        return JsonResponse(400, error);

    if (hub.Status(sid).empty()) return JsonResponse(404, "会话不存在");
    if (!hub.Publish(sid, topic, payload, qos, retain, &error)) {
        return JsonResponse(409, error);
    }
    return Resp(200, "{\"ok\":true}");
}

std::string ClosedFrame(const std::string& reason) {
    nlohmann::json frame;
    frame["state"] = "closed";
    frame["connected"] = false;
    frame["received"] = 0;
    frame["dropped"] = 0;
    frame["lastError"] = reason;
    frame["events"] = nlohmann::json::array();
    return frame.dump(-1, ' ', false,
                      nlohmann::json::error_handler_t::replace);
}

} // namespace

std::pair<int, std::string> HandleMqttProbeApi(
        Northbound::MqttProbeHub& hub, const HttpRequest& req) {
    const std::string pathOnly = req.path.substr(0, req.path.find('?'));
    const std::vector<std::string> seg = SplitPath(pathOnly);

    // 期望: api / mqtt / probe [/ sid [/ publish]]
    if (seg.size() < 3 || seg[0] != "api" || seg[1] != "mqtt"
            || seg[2] != "probe") {
        return JsonResponse(404, "not found");
    }

    if (seg.size() == 3) {
        if (req.method != "POST") return JsonResponse(405, "method not allowed");
        return CreateSession(hub, req.body);
    }

    const std::string& sid = seg[3];
    if (sid.empty()) return JsonResponse(404, "会话不存在");

    if (seg.size() == 4) {
        if (req.method == "GET") {
            const std::string status = hub.Status(sid);
            if (status.empty()) return JsonResponse(404, "会话不存在");
            return Resp(200, status);
        }
        if (req.method == "DELETE") {
            if (!hub.Stop(sid)) return JsonResponse(404, "会话不存在");
            return Resp(200, "{\"ok\":true}");
        }
        return JsonResponse(405, "method not allowed");
    }

    if (seg.size() == 5 && seg[4] == "publish") {
        if (req.method != "POST") return JsonResponse(405, "method not allowed");
        return PublishToSession(hub, sid, req.body);
    }

    return JsonResponse(404, "not found");
}

std::string BuildMqttProbeStream(
        Northbound::MqttProbeHub& hub, const std::string& path) {
    const std::string sid = QueryParam(path, "sid");
    if (sid.empty()) return ClosedFrame("sid 缺失");
    return hub.Poll(sid);
}

}} // namespace MyProt::App

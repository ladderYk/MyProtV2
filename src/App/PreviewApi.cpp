// src/App/PreviewApi.cpp — 试渲染 / 响应解析预览端点
//
//   GET  /api/preview?scope=protocols&name=<n>[&operation=<op>]
//        Render one operation from the protocol currently on disk. Without `operation`
//        only the sorted operation list is returned (onboarding aid).
//   POST /api/preview?scope=protocols&name=<n>
//        body = {
//          "operation": "Read",                       // required for a frame
//          "variables": { "StartByteAddress": 100, "ByteCount": 2 },  // optional overrides
//          "raw": { "Data": "01 02 03" },             // optional {Name:raw} payloads
//          "payloadBytes": 3,                         // dummy size when a raw value is omitted
//          "protocol": "<candidate JSON text>"        // optional: preview unsaved edits
//        }
//
//   /api/preview/response — the other half of protocol onboarding: feed device bytes back
//   through Engine::ResponseParser and show the value/quality the gateway would produce.
//   GET/POST share the same params: {protocol, operation, hex, finalType, bitOffset,
//   byteCount, byteOrder, converters}. GET should use compact hex (no spaces) because query
//   values are not URL-decoded by the router; converters is POST-only.
//
//   converters = tag-level post-read chain copied from tags.json (v5 C1), e.g.
//   [{ "kind": "scale", "k": 0.1, "b": -40 }] -> value' = value*k + b (Double).
//
// Semantics mirror /api/validate: a successfully handled request returns 200 even when
// `ok` is false; the body carries the structured reason. Nothing is written to disk and
// no reload is triggered. The frame is rendered by Service::PreviewProtocolOperation,
// i.e. through the same RequestBuilder pipeline the gateway uses.

#include "RuntimeGlue.hpp"
#include "MyProt/Service/TrialPreview.hpp"
#include "MyProt/Service/ResponsePreview.hpp"
#include "MyProt/Southbound/HttpPuller.hpp"
#include "MyProt/Southbound/PayloadMapper.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace MyProt { namespace App {

namespace {

typedef std::pair<int, std::string> Resp;

Resp ErrorResponse(int code, const std::string& msg) {
    nlohmann::json j;
    j["error"] = msg;
    return std::make_pair(code, j.dump());
}

bool ReadUInt32(const nlohmann::json& v, unsigned int* out) {
    if (!v.is_number_integer() && !v.is_number_unsigned()) return false;
    if (v.is_number_integer() && v.get<long long>() < 0) return false;
    const unsigned long long raw = v.get<unsigned long long>();
    if (raw > 0xFFFFFFFFull) return false;
    *out = static_cast<unsigned int>(raw);
    return true;
}

std::string PreviewJson(const Service::TrialPreviewResult& r) {
    nlohmann::json j;
    j["ok"] = r.ok;
    j["protocol"] = r.protocolName;
    j["operation"] = r.operation;
    j["frameLength"] = r.frameLength;
    j["hex"] = r.frameHex;
    j["ascii"] = r.ascii;
    j["errors"] = r.errors;
    j["warnings"] = r.warnings;
    j["operations"] = r.operations;
    if (!r.errors.empty()) j["error"] = r.errors.front();

    nlohmann::json vars = nlohmann::json::array();
    for (std::size_t i = 0; i < r.variables.size(); ++i) {
        nlohmann::json v;
        v["name"] = r.variables[i].name;
        v["source"] = r.variables[i].source;
        if (r.variables[i].hasValue) {
            v["value"] = static_cast<unsigned long long>(r.variables[i].value);
        } else {
            v["value"] = nullptr;
        }
        vars.push_back(v);
    }
    j["variables"] = vars;

    if (r.hasLengthField) {
        nlohmann::json lf;
        lf["offset"] = r.lengthFieldOffset;
        lf["width"] = r.lengthFieldLength;
        lf["byteOrder"] = r.lengthFieldByteOrder;
        lf["value"] = r.lengthFieldValue;
        lf["expected"] = r.lengthFieldExpected;
        lf["ok"] = r.lengthFieldOk;
        j["lengthField"] = lf;
    } else {
        j["lengthField"] = nullptr;
    }
    return j.dump();
}

/// Tag-level converter chain from the request body: [{kind:"scale", k:<num>, b:<num>}, ...].
/// Shape validation mirrors ProtocolJsonParser so a typo fails here, not silently later.
bool ReadConverters(const nlohmann::json& arr, std::vector<Core::ScaleConverter>* out,
                    std::string* err) {
    if (!arr.is_array()) { *err = "converters 须为数组"; return false; }
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const nlohmann::json& item = arr.at(i);
        const std::string at = "converters[" + std::to_string(i) + "]";
        if (!item.is_object()) { *err = at + " 须为对象"; return false; }
        if (!item.count("kind") || !item.at("kind").is_string()) {
            *err = at + " 须含字符串 kind";
            return false;
        }
        if (item.at("kind").get<std::string>() != "scale") {
            *err = at + ".kind 仅支持 scale";
            return false;
        }
        if (!item.count("k") || !item.at("k").is_number()
            || !item.count("b") || !item.at("b").is_number()) {
            *err = at + ".scale 须含数值 k 与 b";
            return false;
        }
        Core::ScaleConverter conv;
        conv.k = item.at("k").get<double>();
        conv.b = item.at("b").get<double>();
        out->push_back(conv);
    }
    return true;
}

std::string ResponsePreviewJson(const Service::ResponsePreviewResult& r) {
    nlohmann::json j;
    j["ok"] = r.ok;
    j["protocol"] = r.protocolName;
    j["operation"] = r.operation;
    j["errors"] = r.errors;
    j["warnings"] = r.warnings;
    j["operations"] = r.operations;
    if (!r.errors.empty()) j["error"] = r.errors.front();

    nlohmann::json cond;
    cond["checked"] = r.conditionChecked;
    cond["expr"] = r.conditionExpr;
    cond["ok"] = r.conditionOk;
    j["condition"] = cond;

    j["byteOrder"] = r.byteOrder;
    j["dataStartIndex"] = r.dataStartIndex;
    j["frameLength"] = r.frameLength;
    j["rawLength"] = r.rawLength;
    j["rawHex"] = r.rawHex;

    if (r.hasValue) {
        nlohmann::json v;
        v["type"] = r.valueType;
        v["text"] = r.valueText;
        if (r.hasNumeric) v["numeric"] = r.numeric;
        else v["numeric"] = nullptr;
        j["value"] = v;
    } else {
        j["value"] = nullptr;
    }

    nlohmann::json chain = nlohmann::json::array();
    for (std::size_t i = 0; i < r.converters.size(); ++i) {
        nlohmann::json c;
        c["k"] = r.converters[i].k;
        c["b"] = r.converters[i].b;
        chain.push_back(c);
    }
    j["converters"] = chain;
    if (r.hasPreValue) {
        nlohmann::json pre;
        pre["type"] = r.preValueType;
        pre["text"] = r.preValueText;
        if (r.hasPreNumeric) pre["numeric"] = r.preNumeric;
        else pre["numeric"] = nullptr;
        j["valueBeforeConversion"] = pre;
    } else {
        j["valueBeforeConversion"] = nullptr;
    }
    j["quality"] = r.quality;
    j["timestamp"] = r.timestamp;
    return j.dump();
}

/// Query-value integer parse (no URL decoding in the router: decimal digits only).
bool ReadLongText(const std::string& text, long* out) {
    if (text.empty()) return false;
    char* end = 0;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    *out = v;
    return true;
}

} // namespace

std::pair<int, std::string> HandlePreviewApi(const Service::ConfigStore& store,
                                             const HttpRequest& req) {
    if (req.method != "GET" && req.method != "POST") {
        return ErrorResponse(405, "method not allowed");
    }
    const std::string scope = QueryParam(req.path, "scope");
    if (!scope.empty() && scope != "protocols" && scope != "protocol") {
        return ErrorResponse(400, "试渲染仅支持 scope=protocols");
    }
    const std::string name = QueryParam(req.path, "name");

    // ── Candidate protocol text: request body wins over the file on disk ──
    std::string protocolJson;
    Service::TrialPreviewRequest tr;
    bool haveCandidate = false;

    if (req.method == "POST") {
        if (req.body.empty()) return ErrorResponse(400, "请求体为空");
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            return ErrorResponse(400, std::string("请求体不是合法 JSON: ") + e.what());
        }
        if (!body.is_object()) return ErrorResponse(400, "请求体须为 JSON 对象");

        if (body.count("protocol")) {
            const nlohmann::json& p = body.at("protocol");
            if (p.is_string()) {
                protocolJson = p.get<std::string>();
                haveCandidate = true;
            } else if (p.is_object()) {
                protocolJson = p.dump(2);
                haveCandidate = true;
            } else {
                return ErrorResponse(400, "protocol 须为 JSON 文本或对象");
            }
        }
        if (body.count("operation")) {
            if (!body.at("operation").is_string()) {
                return ErrorResponse(400, "operation 须为字符串");
            }
            tr.operation = body.at("operation").get<std::string>();
        }
        if (body.count("variables")) {
            if (!body.at("variables").is_object()) {
                return ErrorResponse(400, "variables 须为对象");
            }
            const nlohmann::json& vars = body.at("variables");
            for (nlohmann::json::const_iterator it = vars.begin(); it != vars.end(); ++it) {
                unsigned int v = 0;
                if (!ReadUInt32(it.value(), &v)) {
                    return ErrorResponse(400, "variables." + it.key()
                        + " 须为 0..4294967295 的整数");
                }
                tr.variables[it.key()] = v;
            }
        }
        if (body.count("raw")) {
            if (!body.at("raw").is_object()) {
                return ErrorResponse(400, "raw 须为对象");
            }
            const nlohmann::json& raw = body.at("raw");
            for (nlohmann::json::const_iterator it = raw.begin(); it != raw.end(); ++it) {
                if (!it.value().is_string()) {
                    return ErrorResponse(400, "raw." + it.key() + " 须为 hex 字符串");
                }
                tr.rawVariables[it.key()] = it.value().get<std::string>();
            }
        }
        if (body.count("payloadBytes")) {
            unsigned int n = 0;
            if (!ReadUInt32(body.at("payloadBytes"), &n) || n > 1024) {
                return ErrorResponse(400, "payloadBytes 须为 0..1024 的整数");
            }
            tr.payloadBytes = static_cast<std::size_t>(n);
        }
    } else if (req.path.find("operation=") != std::string::npos) {
        tr.operation = QueryParam(req.path, "operation");
    }

    if (!haveCandidate) {
        if (name.empty()) return ErrorResponse(400, "缺少 name 参数 (或 POST 携带 protocol)");
        const Core::Expected<std::string> got =
            store.Get(Service::ConfigScope::Protocol, name);
        if (!got.has_value()) return ErrorResponse(404, got.error().message);
        protocolJson = got.value();
    }
    tr.protocolJson = protocolJson;

    return std::make_pair(200, PreviewJson(Service::PreviewProtocolOperation(tr)));
}

std::pair<int, std::string> HandleResponsePreviewApi(const Service::ConfigStore& store,
                                                     const HttpRequest& req) {
    if (req.method != "GET" && req.method != "POST") {
        return ErrorResponse(405, "method not allowed");
    }
    const std::string scope = QueryParam(req.path, "scope");
    if (!scope.empty() && scope != "protocols" && scope != "protocol") {
        return ErrorResponse(400, "响应预览仅支持 scope=protocols");
    }
    const std::string name = QueryParam(req.path, "name");

    Service::ResponsePreviewRequest rp;
    std::string protocolJson;
    bool haveCandidate = false;

    // Query params first: they are the whole input for GET and fall back for POST.
    rp.operation = QueryParam(req.path, "operation");
    rp.hex = QueryParam(req.path, "hex");
    rp.finalType = QueryParam(req.path, "finalType");
    rp.byteOrder = QueryParam(req.path, "byteOrder");
    {
        long v = 0;
        const std::string bit = QueryParam(req.path, "bitOffset");
        if (ReadLongText(bit, &v)) rp.bitOffset = static_cast<int>(v);
        const std::string cnt = QueryParam(req.path, "byteCount");
        if (ReadLongText(cnt, &v)) rp.byteCount = v;
    }

    if (req.method == "POST") {
        if (req.body.empty()) return ErrorResponse(400, "请求体为空");
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (const std::exception& e) {
            return ErrorResponse(400, std::string("请求体不是合法 JSON: ") + e.what());
        }
        if (!body.is_object()) return ErrorResponse(400, "请求体须为 JSON 对象");

        if (body.count("protocol")) {
            const nlohmann::json& p = body.at("protocol");
            if (p.is_string()) {
                protocolJson = p.get<std::string>();
                haveCandidate = true;
            } else if (p.is_object()) {
                protocolJson = p.dump(2);
                haveCandidate = true;
            } else {
                return ErrorResponse(400, "protocol 须为 JSON 文本或对象");
            }
        }
        const char* kStrings[] = { "operation", "hex", "finalType", "byteOrder" };
        for (std::size_t i = 0; i < sizeof(kStrings) / sizeof(kStrings[0]); ++i) {
            if (!body.count(kStrings[i])) continue;
            if (!body.at(kStrings[i]).is_string()) {
                return ErrorResponse(400, std::string(kStrings[i]) + " 须为字符串");
            }
        }
        if (body.count("operation")) rp.operation = body.at("operation").get<std::string>();
        if (body.count("hex"))       rp.hex       = body.at("hex").get<std::string>();
        if (body.count("finalType")) rp.finalType = body.at("finalType").get<std::string>();
        if (body.count("byteOrder")) rp.byteOrder = body.at("byteOrder").get<std::string>();
        if (body.count("bitOffset")) {
            if (!body.at("bitOffset").is_number_integer()) {
                return ErrorResponse(400, "bitOffset 须为整数 (-1 或 0..7)");
            }
            const long long v = body.at("bitOffset").get<long long>();
            if (v < -1 || v > 7) return ErrorResponse(400, "bitOffset 须为 -1 或 0..7");
            rp.bitOffset = static_cast<int>(v);
        }
        if (body.count("byteCount")) {
            unsigned int n = 0;
            if (!ReadUInt32(body.at("byteCount"), &n)) {
                return ErrorResponse(400, "byteCount 须为 0..4294967295 的整数");
            }
            rp.byteCount = static_cast<long>(n);
        }
        if (body.count("converters")) {
            std::string convErr;
            if (!ReadConverters(body.at("converters"), &rp.converters, &convErr)) {
                return ErrorResponse(400, convErr);
            }
        }
    }

    if (!haveCandidate) {
        if (name.empty()) return ErrorResponse(400, "缺少 name 参数 (或 POST 携带 protocol)");
        const Core::Expected<std::string> got =
            store.Get(Service::ConfigScope::Protocol, name);
        if (!got.has_value()) return ErrorResponse(404, got.error().message);
        protocolJson = got.value();
    }
    if (rp.hex.empty()) return ErrorResponse(400, "缺少 hex 响应字节");
    rp.protocolJson = protocolJson;

    return std::make_pair(200, ResponsePreviewJson(Service::PreviewProtocolResponse(rp)));
}

// ── POST /api/preview/pull — 试拉取 (ADR-0023) ──
// Body = one `pull.http` block plus an optional declared-tag list:
//   { "url": "http://...", "method": "GET", "headers": {...}, "body": "...",
//     "timeoutMs": 3000, "tagField": "tag", "valueField": "value", "tsField": "ts",
//     "arrayOfItems": true, "declaredTags": ["METER-01.Temp"] }
// Performs exactly one request through the same client + request builder as the pull
// loop, then maps the payload with PayloadMapper. Read-only: no disk write, no reload,
// no effect on the running binding. HTTP 200 means the preview was handled; `ok` is
// the pull result (see also /api/preview semantics).
std::pair<int, std::string> HandlePullPreviewApi(const Service::ConfigStore& store,
                                                 const HttpRequest& req) {
    (void)store;
    if (req.method != "POST") return ErrorResponse(405, "method not allowed");
    if (req.body.empty()) return ErrorResponse(400, "请求体为空");

    nlohmann::json body;
    try {
        body = nlohmann::json::parse(req.body);
    } catch (const std::exception& e) {
        return ErrorResponse(400, std::string("请求体不是合法 JSON: ") + e.what());
    }
    if (!body.is_object()) return ErrorResponse(400, "请求体须为 JSON 对象");

    Core::HttpPullConfig cfg;
    if (!body.count("url") || !body.at("url").is_string()) {
        return ErrorResponse(400, "url 须为字符串");
    }
    cfg.url = body.at("url").get<std::string>();
    if (cfg.url.empty()) return ErrorResponse(400, "url 不能为空");

    if (body.count("method")) {
        if (!body.at("method").is_string()) return ErrorResponse(400, "method 须为字符串");
        std::string method = body.at("method").get<std::string>();
        for (std::size_t i = 0; i < method.size(); ++i) {
            if (method[i] >= 'a' && method[i] <= 'z') {
                method[i] = static_cast<char>(method[i] - 'a' + 'A');
            }
        }
        if (method != "GET" && method != "POST") {
            return ErrorResponse(400, "method 仅支持 GET / POST");
        }
        cfg.method = method;
    }
    if (body.count("body")) {
        if (!body.at("body").is_string()) return ErrorResponse(400, "body 须为字符串");
        cfg.body = body.at("body").get<std::string>();
    }
    if (body.count("timeoutMs")) {
        unsigned int n = 0;
        if (!ReadUInt32(body.at("timeoutMs"), &n) || n < 100 || n > 60000) {
            return ErrorResponse(400, "timeoutMs 须为 100..60000 的整数");
        }
        cfg.timeoutMs = static_cast<int>(n);
    }
    if (body.count("headers")) {
        if (!body.at("headers").is_object()) return ErrorResponse(400, "headers 须为对象");
        const nlohmann::json& headers = body.at("headers");
        for (nlohmann::json::const_iterator it = headers.begin(); it != headers.end(); ++it) {
            if (!it.value().is_string()) {
                return ErrorResponse(400, "headers." + it.key() + " 须为字符串");
            }
            const std::string value = it.value().get<std::string>();
            const bool injection = it.key().find('\r') != std::string::npos
                || it.key().find('\n') != std::string::npos
                || value.find('\r') != std::string::npos
                || value.find('\n') != std::string::npos;
            if (injection) return ErrorResponse(400, "headers." + it.key() + " 含 CR/LF");
            cfg.headers[it.key()] = value;
        }
    }
    if (body.count("tagField")) {
        if (!body.at("tagField").is_string()) return ErrorResponse(400, "tagField 须为字符串");
        cfg.tagField = body.at("tagField").get<std::string>();
    }
    if (body.count("valueField")) {
        if (!body.at("valueField").is_string()) return ErrorResponse(400, "valueField 须为字符串");
        cfg.valueField = body.at("valueField").get<std::string>();
    }
    if (body.count("tsField")) {
        if (!body.at("tsField").is_string()) return ErrorResponse(400, "tsField 须为字符串");
        cfg.tsField = body.at("tsField").get<std::string>();
    }
    if (body.count("arrayOfItems")) {
        if (!body.at("arrayOfItems").is_boolean()) {
            return ErrorResponse(400, "arrayOfItems 须为布尔值");
        }
        cfg.arrayOfItems = body.at("arrayOfItems").get<bool>();
    }

    std::vector<std::string> declaredTags;
    if (body.count("declaredTags")) {
        if (!body.at("declaredTags").is_array()) {
            return ErrorResponse(400, "declaredTags 须为字符串数组");
        }
        const nlohmann::json& tags = body.at("declaredTags");
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (!tags.at(i).is_string()) {
                return ErrorResponse(400, "declaredTags 须为字符串数组");
            }
            declaredTags.push_back(tags.at(i).get<std::string>());
        }
    }

    const Southbound::HttpPuller::ProbeResult probe = Southbound::HttpPuller::Probe(cfg);

    nlohmann::json j;
    j["ok"] = probe.ok;
    j["status"] = probe.status;
    j["elapsedMs"] = probe.elapsedMs;
    j["bytes"] = static_cast<unsigned long long>(probe.body.size());
    j["error"] = probe.error;
    j["bodyPreview"] = probe.body.substr(0, 4096);

    nlohmann::json mapped = nlohmann::json::array();
    nlohmann::json unmapped = nlohmann::json::array();
    nlohmann::json extra = nlohmann::json::array();
    j["mappingError"] = "";

    if (probe.ok) {
        std::vector<Southbound::MappingItem> items;
        std::string mapErr;
        if (!Southbound::PayloadMapper::MapJson(probe.body, cfg.tagField, cfg.valueField,
                                                cfg.tsField, std::string(), items, mapErr,
                                                cfg.arrayOfItems)) {
            j["mappingError"] = mapErr;
        } else {
            std::set<std::string> mappedTags;
            for (std::size_t i = 0; i < items.size(); ++i) {
                nlohmann::json item;
                item["tag"] = items[i].tagName;
                item["value"] = items[i].value;
                if (items[i].timestampMs != 0) item["ts"] = items[i].timestampMs;
                mapped.push_back(item);
                mappedTags.insert(items[i].tagName);
            }
            for (std::size_t i = 0; i < declaredTags.size(); ++i) {
                if (mappedTags.find(declaredTags[i]) == mappedTags.end()) {
                    unmapped.push_back(declaredTags[i]);
                }
            }
            for (std::set<std::string>::const_iterator it = mappedTags.begin();
                    it != mappedTags.end(); ++it) {
                if (std::find(declaredTags.begin(), declaredTags.end(), *it)
                        == declaredTags.end()) {
                    extra.push_back(*it);
                }
            }
        }
    }
    j["mapped"] = mapped;
    j["unmapped"] = unmapped;
    j["extra"] = extra;
    return std::make_pair(200, j.dump());
}

}} // namespace MyProt::App

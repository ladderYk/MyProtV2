// src/App/ValidateApi.cpp — /api/validate 只读校验端点 (方案 2)
//
//   GET  /api/validate?scope=protocols|tags&name=<n>   校验磁盘上当前内容
//   POST /api/validate?scope=protocols|tags&name=<n>   校验请求体候选内容 (body = JSON 文本)
//
// 响应 (校验未通过也返回 200 — 请求本身成功, 语义见 body.ok):
//   {"ok":bool,"scope":"..","name":"..","errorCount":N,"warningCount":M,
//    "issues":[{"severity","ruleId","subject","field","message"}, ...]}
// 参数缺失/非法 → 400; 方法非 GET/POST → 405; GET 目标配置不存在 → 404
//
// 与 PUT /api/config/... 的差别: 本端点**绝不写盘 / 不备份 / 不触发热重载** — 纯只读。
// 用途 (webui): ① 保存前"自检" ② 保存被拒后取结构化原因 → 点击定位到具体字段。
// 结构化分类见 Service::ClassifyValidationItems (ValidationIssue.hpp);
// ruleId="parse.error" 表示校验器整体失败 (JSON 语法错误等), 非规则表条目。

#include "RuntimeGlue.hpp"
#include "MyProt/Service/ValidationIssue.hpp"

#include <string>
#include <vector>

namespace MyProt { namespace App {

namespace {

/// JSON 字符串字面量转义 (UTF-8 字节原样保留; 仅转义 JSON 必需字符)
std::string JsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string IssueJson(const Service::ValidationIssue& i) {
    std::string s = "{\"severity\":";
    s += JsonString(i.severity);
    s += ",\"ruleId\":";   s += JsonString(i.ruleId);
    s += ",\"subject\":";  s += JsonString(i.subject);
    s += ",\"field\":";    s += JsonString(i.field);
    s += ",\"message\":";  s += JsonString(i.message);
    s += "}";
    return s;
}

bool ScopeFromString(const std::string& s, Service::ConfigScope* out) {
    if (s == "protocols" || s == "protocol") {
        *out = Service::ConfigScope::Protocol;
        return true;
    }
    if (s == "tags") {
        *out = Service::ConfigScope::Tags;
        return true;
    }
    return false;
}

std::pair<int, std::string> ErrorResponse(int code, const std::string& msg) {
    return std::make_pair(code, std::string("{\"error\":") + JsonString(msg) + "}");
}

} // namespace

std::pair<int, std::string> HandleValidateApi(const Service::ConfigStore& store,
                                              const HttpRequest& req) {
    const std::string scopeStr = QueryParam(req.path, "scope");
    const std::string name = QueryParam(req.path, "name");

    Service::ConfigScope scope;
    if (!ScopeFromString(scopeStr, &scope)) {
        return ErrorResponse(400, "scope 须为 protocols 或 tags");
    }
    if (name.empty()) {
        return ErrorResponse(400, "缺少 name 参数");
    }

    // ── 取候选内容: GET = 磁盘现值; POST = 请求体 ──
    std::string payload;
    if (req.method == "GET") {
        const Core::Expected<std::string> got = store.Get(scope, name);
        if (!got.has_value()) return ErrorResponse(404, got.error().message);
        payload = got.value();
    } else if (req.method == "POST") {
        if (req.body.empty()) return ErrorResponse(400, "请求体为空 (POST 需带候选 JSON)");
        payload = req.body;
    } else {
        return ErrorResponse(405, "method not allowed");
    }

    // ── 校验 (与 Save 同一入口, 但无任何副作用) ──
    //   分类器词汇归一: API 用复数 ("protocols" — 与 /api/config/{scope} 一致),
    //   而规则表按域内命名 ("protocol"/"tags") 限定域; 不归一会让协议域规则全部落到
    //   unclassified (表现为前端拿到 ruleId=unclassified, 无法定位)。
    const std::string clsScope = (scopeStr == "tags") ? "tags" : "protocol";

    std::vector<Service::ValidationIssue> issues;
    const Core::Expected<std::vector<std::string> > vr =
        store.Validate(scope, name, payload);
    if (!vr.has_value()) {
        // 校验器整体失败 (JSON 语法错误 / 名称非法等) → 单条 parse.error 回报
        Service::ValidationIssue one;
        one.severity = "error";
        one.ruleId = "parse.error";
        one.scope = scopeStr;
        one.message = vr.error().message;
        issues.push_back(one);
    } else {
        issues = Service::ClassifyValidationItems(vr.value(), clsScope);
    }

    std::string items;
    int errors = 0;
    int warnings = 0;
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (issues[i].severity == "error") ++errors; else ++warnings;
        if (i > 0) items += ",";
        items += IssueJson(issues[i]);
    }

    std::string body = "{\"ok\":";
    body += (errors == 0 ? "true" : "false");
    body += ",\"scope\":";        body += JsonString(scopeStr);
    body += ",\"name\":";         body += JsonString(name);
    body += ",\"errorCount\":";   body += std::to_string(errors);
    body += ",\"warningCount\":"; body += std::to_string(warnings);
    body += ",\"issues\":[";      body += items;
    body += "]}";
    return std::make_pair(200, body);
}

}} // namespace MyProt::App

// src/WebApi/include/MyProt/WebApi/RouteTable.hpp
// 统一路由表（P2-3）— method + 路径模式注册，替代 if 链 / 前缀表 / SSE 特例三套分发。
// 支持 ":name" 段参数；同一路径不同 method 分别注册；Dispatch 区分
// NotFound / MethodNotAllowed，便于上层决定 404、405 或扩展域兜底。
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace MyProt { namespace WebApi {

/// HTTP 响应值类型（原 WebApiServer::Response 上移，路由表与服务器共用）
struct RouteResult {
    int status;
    std::string contentType;
    std::string body;
    bool cacheable;     // 静态资源 true（Vite 带 hash 文件名可缓存）；API 恒 false

    RouteResult() : status(200), contentType("application/json"), cacheable(false) {}
    RouteResult(int s, const std::string& ct, const std::string& b, bool c = false)
        : status(s), contentType(ct), body(b), cacheable(c) {}
};

/// 路由请求上下文（path 含 query；pathOnly 已剥离 query）
struct RouteRequest {
    std::string method;
    std::string path;
    std::string pathOnly;
    std::string body;
    std::string authToken;
    std::map<std::string, std::string> params;

    std::string Param(const std::string& name) const {
        std::map<std::string, std::string>::const_iterator it = params.find(name);
        return it == params.end() ? std::string() : it->second;
    }
};

class RouteTable {
public:
    typedef std::function<RouteResult(const RouteRequest&)> Handler;

    enum MatchResult {
        NotFound,
        MethodNotAllowed,
        Found
    };

    /// 注册路由；pattern 形如 "/api/config/:scope/:name"（按注册顺序匹配，字面量优先于参数）
    void Add(const std::string& method, const std::string& pattern, Handler handler) {
        Route r;
        r.method = method;
        r.pattern = Split(pattern);
        r.display = method + " " + pattern;
        r.handler = handler;
        r.specificity = 0;
        for (size_t i = 0; i < r.pattern.size(); ++i) {
            if (r.pattern[i].empty() || r.pattern[i][0] != ':') ++r.specificity;
        }
        _routes.push_back(r);
    }

    /// 分派请求；Found 时 out 已填充。
    /// 只允许“最具体”的路径模式参与 method 匹配 — 否则 GET /api/config/reload
    /// 会被 /api/config/:scope 吞掉，把应有的 405 变成 404。
    MatchResult Dispatch(RouteRequest& req, RouteResult& out) const {
        const std::vector<std::string> seg = Split(req.pathOnly);
        int best = -1;
        for (size_t i = 0; i < _routes.size(); ++i) {
            std::map<std::string, std::string> params;
            if (!Match(_routes[i].pattern, seg, params)) continue;
            if (_routes[i].specificity > best) best = _routes[i].specificity;
        }
        if (best < 0) return NotFound;

        for (size_t i = 0; i < _routes.size(); ++i) {
            const Route& route = _routes[i];
            if (route.specificity != best || route.method != req.method) continue;
            std::map<std::string, std::string> params;
            if (!Match(route.pattern, seg, params)) continue;
            req.params.swap(params);
            out = route.handler(req);
            return Found;
        }
        return MethodNotAllowed;
    }

    /// 路由清单（"METHOD /pattern"）— 可枚举即可生成 API 清单/做覆盖检查
    std::vector<std::string> Describe() const {
        std::vector<std::string> out;
        out.reserve(_routes.size());
        for (size_t i = 0; i < _routes.size(); ++i) out.push_back(_routes[i].display);
        return out;
    }

private:
    struct Route {
        std::string method;
        std::vector<std::string> pattern;
        std::string display;
        Handler handler;
        int specificity;      // 字面量段数；越大越具体
    };

    static std::vector<std::string> Split(const std::string& path) {
        std::vector<std::string> parts;
        std::string cur;
        for (size_t i = 0; i <= path.size(); ++i) {
            if (i == path.size() || path[i] == '/') {
                if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
            } else {
                cur += path[i];
            }
        }
        return parts;
    }

    static bool Match(const std::vector<std::string>& pattern,
                      const std::vector<std::string>& seg,
                      std::map<std::string, std::string>& params) {
        if (pattern.size() != seg.size()) return false;
        for (size_t i = 0; i < pattern.size(); ++i) {
            const std::string& p = pattern[i];
            if (!p.empty() && p[0] == ':') {
                params[p.substr(1)] = seg[i];
            } else if (p != seg[i]) {
                return false;
            }
        }
        return true;
    }

    std::vector<Route> _routes;
};

}} // namespace MyProt::WebApi

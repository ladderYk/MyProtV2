// src/App/HttpRouter.hpp — WebApi 扩展路由注册表 (header-only)
// 新增接口 = 一行 Add(prefix, handler)。分发按注册顺序前缀匹配:
//   - 先注册者优先 ("​/api/data/write" 须先于 "/api/data")
//   - 方法不匹配 (405) 由各 handler 自行判定 — 与既有行为一致
//   - 无命中返回统一 404 {"error":"not found"}
#ifndef MYPROT_APP_HTTPROUTER_HPP
#define MYPROT_APP_HTTPROUTER_HPP

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "AppContext.hpp"

namespace MyProt { namespace App {

typedef std::pair<int, std::string> HttpResponse;

class HttpRouter {
public:
    typedef std::function<HttpResponse(const AppContext&, const HttpRequest&)>
        RouteHandler;

    /// 注册路由 (前缀匹配, 先注册者优先)
    void Add(const std::string& pathPrefix, RouteHandler handler) {
        Route r;
        r.prefix = pathPrefix;
        r.handler = handler;
        _routes.push_back(r);
    }

    /// 按注册顺序分发; 无命中 → 404
    HttpResponse Dispatch(const AppContext& ctx, const HttpRequest& req) const {
        for (std::size_t i = 0; i < _routes.size(); ++i) {
            if (req.path.rfind(_routes[i].prefix, 0) == 0) {
                return _routes[i].handler(ctx, req);
            }
        }
        return HttpResponse(404, "{\"error\":\"not found\"}");
    }

private:
    struct Route {
        std::string prefix;
        RouteHandler handler;
    };
    std::vector<Route> _routes;
};

}} // namespace MyProt::App

#endif // MYPROT_APP_HTTPROUTER_HPP

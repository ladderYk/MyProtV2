// src/App/HttpRouter.hpp - WebApi extension-route registry (header-only)
// Adding an interface = one line Add(prefix, handler). Dispatch matches prefixes in registration order:
//   - earlier-registered wins ("/api/data/write" must be registered before "/api/data")
//   - method mismatch (405) is decided by each handler itself - consistent with existing behavior
//   - no hit returns a unified 404 {"error":"not found"}
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

    /// Register a route (prefix match, earlier-registered wins)
    void Add(const std::string& pathPrefix, RouteHandler handler) {
        Route r;
        r.prefix = pathPrefix;
        r.handler = handler;
        _routes.push_back(r);
    }

    /// Dispatch in registration order; no hit -> 404
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

// src/WebApi/include/MyProt/WebApi/AuthMiddleware.hpp
// 认证中间件 — token 校验 + 限流 (ADR-0008)

#pragma once
#include <string>
#include <chrono>
#include <mutex>

namespace MyProt { namespace WebApi {

/// 令牌桶限流器
class RateLimiter {
public:
    RateLimiter(int rps, int burst);

    /// 尝试获取一个令牌
    /// @return true = 允许, false = 429 Too Many Requests
    bool TryAcquire();

private:
    int _rps;
    int _burst;
    double _tokens;
    std::chrono::steady_clock::time_point _lastRefill;
    std::mutex _mutex;
};

/// 认证中间件 — token 常量时间比较 (ADR-0008 §3)
class AuthMiddleware {
public:
    /// @param expectedToken 从环境变量 MYPROT_API_TOKEN 读取
    explicit AuthMiddleware(const std::string& expectedToken);

    /// 验证请求 token (常量时间比较, 防时序攻击)
    bool Validate(const std::string& providedToken) const;

private:
    std::string _expectedToken;
};

}} // namespace MyProt::WebApi

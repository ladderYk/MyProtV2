// src/WebApi/src/AuthMiddleware.cpp — 认证中间件实现占位
#include "MyProt/WebApi/AuthMiddleware.hpp"

namespace MyProt { namespace WebApi {

// ── RateLimiter ──

RateLimiter::RateLimiter(int rps, int burst)
    : _rps(rps), _burst(burst), _tokens(static_cast<double>(burst))
    , _lastRefill(std::chrono::steady_clock::now()) {}

bool RateLimiter::TryAcquire() {
    std::lock_guard<std::mutex> lock(_mutex);
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - _lastRefill).count();
    _tokens += elapsed * _rps;
    if (_tokens > static_cast<double>(_burst)) _tokens = static_cast<double>(_burst);
    _lastRefill = now;
    if (_tokens >= 1.0) {
        _tokens -= 1.0;
        return true;
    }
    return false;
}

// ── AuthMiddleware ──

AuthMiddleware::AuthMiddleware(const std::string& expectedToken)
    : _expectedToken(expectedToken) {}

bool AuthMiddleware::Validate(const std::string& providedToken) const {
    // 常量时间比较, 防时序攻击 (ADR-0008 §3)
    if (providedToken.size() != _expectedToken.size()) return false;
    volatile int result = 0;
    for (size_t i = 0; i < providedToken.size(); ++i) {
        result |= (providedToken[i] ^ _expectedToken[i]);
    }
    return result == 0;
}

}} // namespace MyProt::WebApi

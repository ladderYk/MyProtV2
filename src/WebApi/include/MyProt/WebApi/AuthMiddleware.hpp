// src/WebApi/include/MyProt/WebApi/AuthMiddleware.hpp
// Auth middleware - token check + rate limiting (ADR-0008)

#pragma once
#include <string>
#include <chrono>
#include <mutex>

namespace MyProt { namespace WebApi {

/// Token-bucket rate limiter
class RateLimiter {
public:
    RateLimiter(int rps, int burst);

    /// Try to acquire one token
    /// @return true = allowed, false = 429 Too Many Requests
    bool TryAcquire();

private:
    int _rps;
    int _burst;
    double _tokens;
    std::chrono::steady_clock::time_point _lastRefill;
    std::mutex _mutex;
};

/// Auth middleware - constant-time token comparison (ADR-0008 §3)
class AuthMiddleware {
public:
    /// @param expectedToken read from the environment variable MYPROT_API_TOKEN
    explicit AuthMiddleware(const std::string& expectedToken);

    /// Validate a request token (constant-time comparison, guards against timing attacks)
    bool Validate(const std::string& providedToken) const;

private:
    std::string _expectedToken;
};

}} // namespace MyProt::WebApi

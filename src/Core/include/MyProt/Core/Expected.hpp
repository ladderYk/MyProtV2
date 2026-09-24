// src/Core/include/MyProt/Core/Expected.hpp
// Hand-written Expected<T> — monadic error handling, C++11, zero dependencies (ADR-0010 §3)
// Constraint: T must be default-constructible

#pragma once
#include <cassert>
#include <string>
#include <utility>

namespace MyProt { namespace Core {

/// Error type — the Single Source of Truth for the whole project.
/// architecture/04 references this definition only via an annotation table; it no
/// longer re-declares the enum.
struct Error {
    enum class Code {
        // ──── Retryable + idempotent-safe (only read ops trigger retry, see IsRetryable) ────
        Timeout,              // network/device timeout → retry with backoff
        ConnectionRefused,    // connection refused → wait and reconnect
        ConnectionClosed,     // connection dropped → auto-reconnect
        Busy,                 // device busy → retry later

        // ──── Not retryable (mark Bad quality) ────
        InvalidResponse,      // response fails validCondition
        ParseError,           // response parse failed
        BuildError,           // request build failed (Fail-Fast, logged)
        TypeConversionError,  // raw bytes → finalType conversion failed

        // ──── Write-op errors (not retryable; writes are non-idempotent) ────
        WriteTimeout,         // write timeout → no retry, avoid duplicate write
        WriteFailed,          // write failed → no retry
        ReadBackMismatch,     // P1 B post-write read-back mismatch → no retry (avoid duplicate write)

        // ──── Config errors (Fail-Fast at startup) ────
        ConfigError,          // JSON parse/validation failed
        DeviceNotFound,       // device config missing
        TagNotFound,          // tag not found
        ProtocolNotFound,     // invalid protocol name
        CircuitOpen,          // circuit breaker open → reject request

        // ──── Internal errors ────
        InternalError,        // internal error that should never happen
        NotImplemented        // unimplemented feature (now only the TlsChannel stub; write path is implemented, see ADR-0007)
    };

    Code code;
    std::string message;
    std::string context;   // extra context (e.g. "tag=Temperature, device=PLC1")

    static Error Make(Code c, const std::string& msg = "", const std::string& ctx = "") {
        Error e;
        e.code = c;
        e.message = msg;
        e.context = ctx;
        return e;
    }
};

/// Whether an error code is retryable (read ops: Timeout/ConnectionRefused/ConnectionClosed/Busy)
inline bool IsRetryable(Error::Code c) {
    switch (c) {
        case Error::Code::Timeout:
        case Error::Code::ConnectionRefused:
        case Error::Code::ConnectionClosed:
        case Error::Code::Busy:
            return true;
        default:
            return false;
    }
}

/// KI-04: whether an error triggers a Degraded device-lifecycle transition (connection-level faults).
/// Protocol/business-level errors (InvalidResponse/ParseError/BuildError/TagNotFound/
/// ProtocolNotFound/DeviceNotFound/CircuitOpen) do not touch the lifecycle.
inline bool IsLifecycleDegrading(Error::Code c) {
    switch (c) {
        case Error::Code::Timeout:
        case Error::Code::ConnectionRefused:
        case Error::Code::ConnectionClosed:
        case Error::Code::Busy:
        case Error::Code::InternalError:  // factory not injected / channel creation failed — treated as connection-layer
            return true;
        default:
            return false;
    }
}

/// Unexpected marker — implicitly builds any Expected<T> from an Error
struct UnexpectedType { Error error; };

inline UnexpectedType Unexpected(Error::Code code,
                                 const std::string& msg = "",
                                 const std::string& ctx = "") {
    UnexpectedType u;
    u.error = Error::Make(code, msg, ctx);
    return u;
}

/// Hand-written Expected (replaces tl::expected / std::expected; ADR-0010 §3).
/// Constraint: T must be default-constructible.
template <typename T>
class Expected {
public:
    Expected(const T& v) : _hasValue(true), _value(v) {}
    Expected(T&& v) : _hasValue(true), _value(std::move(v)) {}
    Expected(const UnexpectedType& u) : _hasValue(false), _error(u.error) {}
    Expected(UnexpectedType&& u) : _hasValue(false), _error(std::move(u.error)) {}

    bool has_value() const { return _hasValue; }
    explicit operator bool() const { return _hasValue; }

    T& value() { assert(_hasValue); return _value; }
    const T& value() const { assert(_hasValue); return _value; }
    Error& error() { assert(!_hasValue); return _error; }
    const Error& error() const { assert(!_hasValue); return _error; }

    T value_or(const T& fallback) const { return _hasValue ? _value : fallback; }

    // ── monadic combinators (C++11; lambda parameter types must be written explicitly, no generic lambdas) ──

    /// F: (T&) -> Expected<U>; on error, short-circuits and propagates the error
    template <typename F>
    auto and_then(F f) -> decltype(f(std::declval<T&>())) {
        if (_hasValue) { return f(_value); }
        return UnexpectedType{_error};
    }

    /// F: (T&) -> T; transforms the value only, errors short-circuit
    template <typename F>
    Expected<T> map(F f) {
        if (_hasValue) { return Expected<T>(f(_value)); }
        return UnexpectedType{_error};
    }

    /// On error, replace with a fallback value
    Expected<T> or_else(const T& fallback) const {
        if (_hasValue) { return *this; }
        return Expected<T>(fallback);
    }

private:
    bool _hasValue;
    T _value;      // valid when _hasValue
    Error _error;  // valid when !_hasValue
};

/// void specialization
template <>
class Expected<void> {
public:
    Expected() : _hasValue(true) {}
    Expected(const UnexpectedType& u) : _hasValue(false), _error(u.error) {}
    Expected(UnexpectedType&& u) : _hasValue(false), _error(std::move(u.error)) {}

    bool has_value() const { return _hasValue; }
    explicit operator bool() const { return _hasValue; }
    Error& error() { assert(!_hasValue); return _error; }
    const Error& error() const { assert(!_hasValue); return _error; }

private:
    bool _hasValue;
    Error _error;
};

// convenience alias
using VoidExpected = Expected<void>;

}} // namespace MyProt::Core

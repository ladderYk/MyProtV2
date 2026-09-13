// src/Core/include/MyProt/Core/Expected.hpp
// 自写 Expected<T> — monadic error handling, C++11, 零依赖 (ADR-0010 §3)
// 约束: T 可默认构造

#pragma once
#include <cassert>
#include <string>
#include <utility>

namespace MyProt { namespace Core {

/// 错误类型 — 全项目唯一事实来源 (Single Source of Truth)。
/// architecture/04 仅以注解表引用本定义，不再重复定义 enum。
struct Error {
    enum class Code {
        // ──── 可重试 + 幂等安全 (仅读操作触发重试, 见 IsRetryable) ────
        Timeout,              // 网络/设备超时 → 退避重试
        ConnectionRefused,    // 连接被拒 → 等待重连
        ConnectionClosed,     // 连接中断 → 自动重连
        Busy,                 // 设备正忙 → 稍后重试

        // ──── 不可重试 (标记 Bad Quality) ────
        InvalidResponse,      // 响应不符合 validCondition
        ParseError,           // 响应解析失败
        BuildError,           // 请求构建失败 (Fail-Fast, 记录日志)
        TypeConversionError,  // 原始字节 → finalType 转换失败

        // ──── 写操作错误 (不可重试, 写非幂等) ────
        WriteTimeout,         // 写超时 → 不重试, 避免重复写入
        WriteFailed,          // 写失败 → 不重试
        ReadBackMismatch,     // P1 B 写后读回不一致 → 不可重试 (避免重复写入)

        // ──── 配置错误 (启动期 Fail-Fast) ────
        ConfigError,          // JSON 解析/校验失败
        DeviceNotFound,       // 设备配置不存在
        TagNotFound,          // 标签未找到
        ProtocolNotFound,     // 协议名无效
        CircuitOpen,          // 熔断器打开 → 拒绝请求

        // ──── 内部错误 ────
        InternalError,        // 不应发生的内部错误
        NotImplemented        // 未实现的功能 (现仅 TlsChannel stub 使用; 写路径已实现, 见 ADR-0007 实现销账)
    };

    Code code;
    std::string message;
    std::string context;   // 附加上下文 (如 "tag=Temperature, device=PLC1")

    static Error Make(Code c, const std::string& msg = "", const std::string& ctx = "") {
        Error e;
        e.code = c;
        e.message = msg;
        e.context = ctx;
        return e;
    }
};

/// 判断错误码是否可重试 (读操作 Timeout/ConnectionRefused/ConnectionClosed/Busy)
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

/// KI-04: 判断错误是否触发设备生命周期 Degraded 迁移 (连接级故障)
/// 协议级 / 业务级 (InvalidResponse/ParseError/BuildError/TagNotFound/
/// ProtocolNotFound/DeviceNotFound/CircuitOpen) 不动生命周期。
inline bool IsLifecycleDegrading(Error::Code c) {
    switch (c) {
        case Error::Code::Timeout:
        case Error::Code::ConnectionRefused:
        case Error::Code::ConnectionClosed:
        case Error::Code::Busy:
        case Error::Code::InternalError:  // 工厂未注入/通道创建失败 — 视为连接层
            return true;
        default:
            return false;
    }
}

/// Unexpected 标记 — 从 Error 隐式构造任意 Expected<T>
struct UnexpectedType { Error error; };

inline UnexpectedType Unexpected(Error::Code code,
                                 const std::string& msg = "",
                                 const std::string& ctx = "") {
    UnexpectedType u;
    u.error = Error::Make(code, msg, ctx);
    return u;
}

/// 自写 Expected (取代 tl::expected / std::expected; ADR-0010 §3)。
/// 约束: T 可默认构造。
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

    // ── monadic 组合子 (C++11; lambda 参数类型须显式书写, 禁泛型 lambda) ──

    /// F: (T&) -> Expected<U>; 出错则短路传递错误
    template <typename F>
    auto and_then(F f) -> decltype(f(std::declval<T&>())) {
        if (_hasValue) { return f(_value); }
        return UnexpectedType{_error};
    }

    /// F: (T&) -> T; 仅变换值, 错误短路
    template <typename F>
    Expected<T> map(F f) {
        if (_hasValue) { return Expected<T>(f(_value)); }
        return UnexpectedType{_error};
    }

    /// 出错时替换为降级值
    Expected<T> or_else(const T& fallback) const {
        if (_hasValue) { return *this; }
        return Expected<T>(fallback);
    }

private:
    bool _hasValue;
    T _value;      // _hasValue 时有效
    Error _error;  // !_hasValue 时有效
};

/// void 特化
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

// 便捷别名
using VoidExpected = Expected<void>;

}} // namespace MyProt::Core

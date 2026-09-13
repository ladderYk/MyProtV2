# MyProtV2 — Core 模块

> **所属**: MyProtV2 模块设计系列
> **下一节**: [Engine 模块](./02_Engine.md)

---

**定位**: INTERFACE 库，纯头文件，**纯 C++11**、零第三方依赖（错误处理由自写 `Expected.hpp` 提供；可选值由自写 `Optional.hpp` 提供——2026-08-29 曾短暂切换 `std::optional` 即回退，见 [ADR-0010 §3 二度回退说明](../adr/0010-vs2015-cpp11-toolchain.md)；字节视图由自写 `ByteView.hpp` 提供，ADR-0010 §3）。

> ⚠️ 配置相关 POCO 以 [Config_Schema.md](../Config_Schema.md) 为唯一事实来源；本文档与之冲突时以 Schema 定稿为准。

## 1.1 Expected<T>（自写，C++11）

**自写 `Expected<T>`** 做 monadic error handling。原设计采用 `tl::expected`，因 VS2015 编译风险弃用（ADR-0010 §3/§4）；本实现为 header-only、零依赖，值与错误并排存放（约定**T 可默认构造**，项目内全部 T 均满足），规避 union/aligned_storage 在老编译器上的边角问题。

```cpp
// src/Core/include/MyProt/Core/Expected.hpp

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
        Timeout,              // 网络/设备超时 — 退避重试
        ConnectionRefused,    // 连接被拒 — 等待重连
        ConnectionClosed,     // 连接中断 — 自动重连
        Busy,                 // 设备正忙 — 稍后重试

        // ──── 不可重试 (标记 Bad Quality) ────
        InvalidResponse,      // 响应不符合 validCondition
        ParseError,           // 响应解析失败
        BuildError,           // 请求构建失败 (Fail-Fast, 记录日志)
        TypeConversionError,  // 原始字节 → finalType 转换失败

        // ──── 写操作错误 (不可重试, 写非幂等) ────
        WriteTimeout,         // 写超时 — 不重试, 避免重复写入
        WriteFailed,          // 写失败 — echo 校验不通过; 不重试, 避免重复写入
        ReadBackMismatch,     // 写应答 echo 校验通过, 但写后读回值与下发值不一致 — 不可重试, HTTP 503
                              //  (P1 B read-back 实装, 2026-08-28; ADR-0007 销账; ADR-0006 → Bad 映射)

        // ──── 配置错误 (启动期 Fail-Fast) ────
        ConfigError,          // JSON 解析/校验失败
        DeviceNotFound,       // 设备配置不存在
        TagNotFound,          // 标签未找到
        ProtocolNotFound,     // 协议名无解
        CircuitOpen,          // 熔断器打开 — 拒绝请求

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

/// 判断错误码是否可重试 (读操作: Timeout/ConnectionRefused/ConnectionClosed/Busy)
/// 写操作三态(WriteTimeout/WriteFailed/ReadBackMismatch)均不可重试, 因写非幂等
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

/// 错误码语义矩阵（4 分类 × 触发场景 / IsRetryable / Quality 映射 / 主要生产模块）
inline const char* ErrorCategoryName(Error::Code c) {
    switch (c) {
        case Error::Code::Timeout:
        case Error::Code::ConnectionRefused:
        case Error::Code::ConnectionClosed:
        case Error::Code::Busy:
            return "RetryableTransport";  // 传输层重试族
        case Error::Code::InvalidResponse:
        case Error::Code::ParseError:
        case Error::Code::BuildError:
        case Error::Code::TypeConversionError:
            return "DataShapeError";      // 数据形态/语义错误族
        case Error::Code::WriteTimeout:
        case Error::Code::WriteFailed:
        case Error::Code::ReadBackMismatch:
            return "WriteError";          // 写错误族（不可重试）
        case Error::Code::ConfigError:
        case Error::Code::DeviceNotFound:
        case Error::Code::TagNotFound:
        case Error::Code::ProtocolNotFound:
        case Error::Code::CircuitOpen:
            return "LookupOrStateError";  // 查找/状态错误族
        default:
            return "InternalError";       // InternalError / NotImplemented
    }
}

/// Error::Code → HTTP 状态码映射（WebApi 层专用，呼应 [modules/07_WebApi §7.5]）
///
/// | Error::Code                  | HTTP | 备注                                  |
/// |------------------------------|------|---------------------------------------|
/// | `ConfigError`                | 500  | 启动期 Fail-Fast, 不应在运行期出现       |
/// | `DeviceNotFound`             | 404  | URL 路径 deviceId 不存在                 |
/// | `TagNotFound`                | 404  | body.tagName 不在配置中                  |
/// | `ProtocolNotFound`           | 500  | 配置缺失（启动期应已校验, 不应到运行期）   |
/// | `CircuitOpen`                | 503  | Retry-After: cooldownMs                |
/// | `Timeout`                    | 504  | 上游设备超时（读)                         |
/// | `ConnectionRefused`          | 502  | 上游连接被拒（运行期突发）                 |
/// | `ConnectionClosed`           | 502  | 上游连接中断                              |
/// | `Busy`                       | 503  | Retry-After: backoff 提示                |
/// | `InvalidResponse`            | 502  | 上游回了脏数据                            |
/// | `ParseError`                 | 502  | 协议解析失败                              |
/// | `BuildError`                 | 500  | 内部构建错误（按 InternalError 处理）      |
/// | `TypeConversionError`        | 502  | finalType 转换失败                       |
/// | `WriteTimeout`               | 504  | 写超时                                   |
/// | `WriteFailed`                | 502  | echo 校验不通过                          |
/// | `ReadBackMismatch`           | 503  | P1 B 回读不一致（不可重试, 503 触发告警）  |
/// | `InternalError`              | 500  | 不应发生, 触发 PagerDuty                |
/// | `NotImplemented`             | 501  | v1 未实现 (TlsChannel stub)            |

/// Unexpected 标记 — 与 Error 隐式构造任何 Expected<T>
struct UnexpectedType { Error error; };

inline UnexpectedType Unexpected(Error::Code code,
                                 const std::string& msg = "",
                                 const std::string& ctx = "") {
    UnexpectedType u;
    u.error = Error::Make(code, msg, ctx);
    return u;
}

/// Error::context 字段注入约定与脱敏规则
///
/// context 用于附加定位信息（"tag=Temperature, device=PLC1"），便于日志反查。
/// 约定：
/// - 格式：key=value 列表，逗号分隔，禁止包含换行 / 控字符（避免日志注入）。
/// - 长度上限：256 字节（过长截断，避免日志体积失控）。
/// - **脱敏字段**（写入 context 前**必须**过滤）：
///   * `password` / `passwd` / `secret` / `token` / `apiKey` / `sessionId` →
///     **不写入 context，也不写入 message**。如必须诊断，统一替换为 `***`。
///   * 设备协议级 `username` / `password` 在握手阶段作为模板变量注入，**不**进入 context。
/// - 序列化：ConsoleLogger 输出时 `Error.code` → 字符串名（如 `Timeout`），`message` 原文，
///   `context` 原文（已过滤），无结构化字段索引。
///
/// 实现位于 `Error::SanitizeContext`（位于 01_Core 内部 helper），由 `Error::Make`
/// 出口处自动调用；调用方无需手动脱敏（除非直接构造 `Error` 而非走 `Make`）。

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

    /// Expected<T> 接口契约表（vs tl::expected / std::expected）
    ///
    /// | 接口                | 自写实现                | tl::expected            | 差异                          |
    /// |---------------------|-------------------------|-------------------------|-------------------------------|
    /// | 隐式构造 T          | ✓ (T& / T&&)            | ✓ (T& / T&&)            | 一致                          |
    /// | 隐式构造 Unexpected | ✓ (UnexpectedType)      | ✓ (Error)               | 1 层包装 (UnexpectedType) 避免歧义 |
    /// | `has_value()`       | ✓                       | ✓                       | 一致                          |
    /// | `operator bool()`   | ✓ **explicit**          | ✓ explicit (C++17)      | 自写永远 explicit, 防 if 误用   |
    /// | `value()` (非 const) | ✓ assert(!err) 跳出     | ✓ throw bad_expected_access | assert 优于 throw (零开销) |
    /// | `value()` (const)   | ✓                       | ✓ throw                 | 同上                          |
    /// | `error()`           | ✓ assert(value)         | ✓ throw                 | 同上                          |
    /// | `value_or(default)` | ✓                       | ✓                       | 一致                          |
    /// | and_then / or_else  | ✗ (未实现)              | ✓ (C++23 std::expected)  | 自写刻意不引入 monadic 链式组合 |
    /// | `*` / `->` 访问     | ✗ (未实现)              | ✓                       | 必须走 .value() (防误读未生效分支) |
    /// | 拷贝 T 不满足默认   | 编译错 (static_assert)  | 编译错                  | 自写硬约束: T 可默认构造        |
    /// | 移动 T 不满足默认   | 同上                    | 同上                    | 同上                          |
    /// | 异常说明            | 无 (仅 assert)          | throw_on_default_construct 区别 | 项目代码不抛异常, 故 assert 足够 |
    ///
    /// **不可使用场景**：
    /// 1. T = 引用类型 → T 可默认构造无意义, 编译错。
    /// 2. T 含 `const` 成员 → 与"可默认构造"冲突, 编译错。
    /// 3. T 体积大 + 频繁失败路径 → 仍按值存放, 不优化 (EBO 不适用)。
    /// 4. C++17 `if (e)` 不支持隐式 bool → 必须 `if (e.has_value())` 或 `if (!!e)`。
    /// 5. 跨线程共享 Expected → 不安全 (T 拷贝需同步), 调用方负责同步。

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

}} // namespace MyProt { namespace Core

// ====== 使用示例 ======
//
// // 推荐: 显式判断式 (控制流清晰, VS2015 友好)
// Expected<std::vector<uint8_t>> resp = /* SendReceive 结果 */;
// if (!resp) { /* resp.error().code / message / context */ return; }
// use(resp.value());
//
// // monadic 链式 (lambda 参数类型显式; 禁止泛型 lambda)
// Expected<int> result = ParseResponse(data)
//     .and_then([](std::vector<uint8_t>& bytes) { return ConvertType(bytes); })
//     .map([](int& v) { return v * 2; })
//     .or_else(-1);  // 出错降级
```

## 1.1.1 Optional<T> 与 ByteView（自写）

> `Core::Optional<T>` 为自研实现（VS2015 / C++11 无 `std::optional`，见 [ADR-0010](../adr/0010-vs2015-cpp11-toolchain.md)）。
> ByteView 沿用自写（零依赖，与 `std::span` 替代关系稳定）。

自写 `Optional<T>` 与 `ByteView`，均 header-only、零依赖（ADR-0010 §3）。

```cpp
// src/Core/include/MyProt/Core/Optional.hpp

#pragma once
#include <cassert>
#include <utility>

namespace MyProt { namespace Core {

/// 自写 Optional (取代 std::optional; ADR-0010 §3)。约定: T 可默认构造。
template <typename T>
class Optional {
public:
    Optional() : _hasValue(false) {}
    Optional(const T& v) : _hasValue(true), _value(v) {}
    Optional(T&& v) : _hasValue(true), _value(std::move(v)) {}

    // 显式提供拷贝/移动, 避免 T 赋值重载抑制隐式生成
    // (否则含 Optional 成员的结构体整对象拷贝赋值不可用)
    Optional(const Optional& o) : _hasValue(o._hasValue), _value(o._value) {}
    Optional& operator=(const Optional& o) {
        if (this != &o) { _hasValue = o._hasValue; _value = o._value; }
        return *this;
    }
    Optional(Optional&& o) : _hasValue(o._hasValue), _value(std::move(o._value)) {}
    Optional& operator=(Optional&& o) {
        if (this != &o) { _hasValue = o._hasValue; _value = std::move(o._value); }
        return *this;
    }

    bool hasValue() const { return _hasValue; }
    explicit operator bool() const { return _hasValue; }

    T& value() { assert(_hasValue); return _value; }
    const T& value() const { assert(_hasValue); return _value; }
    T valueOr(const T& fallback) const { return _hasValue ? _value : fallback; }

    void reset() { _hasValue = false; _value = T(); }
    void operator=(const T& v) { _value = v; _hasValue = true; }
    void operator=(T&& v) { _value = std::move(v); _hasValue = true; }

private:
    bool _hasValue;
    T _value;   // _hasValue 时有效
};

}} // namespace MyProt { namespace Core
```

```cpp
// src/Core/include/MyProt/Core/ByteView.hpp

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MyProt { namespace Core {

using Bytes = std::vector<uint8_t>;

/// 轻量字节视图 (取代 std::span<const uint8_t>; ADR-0010 §3)。不持有数据,
/// 生命周期由调用方保证 (通道内部缓冲 / 请求帧在 SendReceive 期间存活)。
struct ByteView {
    const uint8_t* data;
    size_t size;

    ByteView() : data(0), size(0) {}
    ByteView(const uint8_t* d, size_t n) : data(d), size(n) {}
    ByteView(const Bytes& b) : data(b.data()), size(b.size()) {}

    bool empty() const { return size == 0; }
    const uint8_t& operator[](size_t i) const { return data[i]; }
};

}} // namespace MyProt { namespace Core
```

## 1.2 Config POCO — tagged struct 强类型配置（C++11）

> 判别联合体（`framing` / `transport`）以 **tagged struct** 表达：顶层 `type` 枚举对应 JSON 判别字段，各分支字段平铺，仅生效分支可读。取代 `std::variant`（ADR-0010 §3）。

**JSON ↔ tagged struct 一对一映射示例**（以 `FramingConfig` 为例）：

```json
// 配置文件: configs/protocols/modbus-tcp.json
{
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 4,
    "lengthFieldLength": 2,
    "lengthIncludesHeader": false,
    "byteOrder": "BigEndian",
    "headerLength": 6,
    "lengthAdjustment": 0,
    "maxFrameSize": 1024
  }
}
```

```cpp
// 反序列化后 (Pseudo):
FramingConfig f;
f.type = FramingType::LengthField;          // ← 来自 "type":"LengthField"
f.lengthField.lengthFieldOffset = 4;        // ← 来自 "lengthFieldOffset":4
f.lengthField.lengthFieldLength = 2;        // ← 来自 "lengthFieldLength":2
f.lengthField.lengthIncludesHeader = false;
f.lengthField.byteOrder = ByteOrder::BigEndian;
f.lengthField.headerLength = 6;
f.lengthField.lengthAdjustment = 0;
f.lengthField.maxFrameSize = 1024;
// 注: f.fixed / f.silence / f.message 字段未实装, 始终保持默认值 (未读取)
```

**反序列化纪律**：
- `type` 字符串 → 枚举映射走 `FramingTypeParse` / `TransportTypeParse`（拒绝未知值）。
- 同级其他字段（无论是否属于 type 分支）一律反序列化；运行时仅按 type 读取生效字段。
- 未知字段（如 `"futureField": 1`）→ 加载器给出 **Warning**（不阻断），便于后续扩展识别新字段。

**判别联合体字段读取约束（v1 强约束）**：

- `FramingConfig::type` 与 `TransportConfig::type` 是**唯一运行时判别源**，**绝不**靠"字段是否非零"推断（大量配置字段默认值为 0，无法区分"未设置"与"显式为 0"）。
- 读取未生效分支字段 = 未定义行为（C++11 无 `std::variant` 的 hold-alternative 检查）。配置加载器在 ConfigValidator 阶段按 type 校验必填字段；运行期框架代码（TagReader / IChannel 构造）只走 type 派发，**不会**误读其他分支。
- **运行时 fail-safe**：TagReader 入口对 `op.framing.type` 二次校验（如 `framing == FramingType::Message` 即报 `ConfigError`，因 v1 不实现 CAN），返回 `ConfigError` 错误码并停止该操作（不进轮询循环）。
- **扩展原则**：新增 `framing` 分支需同时改 `FramingType` 枚举、`FramingConfig` 平铺字段、ConfigValidator 必填校验、TagReader 派发分支四处；缺一即编译/运行期失败（强类型纪律）。

**字段取用矩阵**（`FramingConfig` / `TransportConfig` 的 type → 取用字段）：

| 配置 | type | 有效字段 | 加载期校验 | 运行期消费者 |
|------|------|----------|-----------|--------------|
| `FramingConfig` | `LengthField` | `lengthField.*` | `lengthFieldLength` ∈ {1,2,4} | `TcpChannel` / `TlsChannel` |
| `FramingConfig` | `Fixed` | `fixed.fixedLength > 0` | `fixedLength` 必须 > 0 | 同上 |
| `FramingConfig` | `Silence` | `silence.*` | `frameGapUs > 0` 或允许 `0 = 3.5×charTimeUs` | `SerialChannel`（v1） |
| `FramingConfig` | `Message` | `message.*` | 校验器报名称预留（CAN 未实装） | 见 [ROADMAP.md](../ROADMAP.md) |
| `TransportConfig` | `Tcp` | `tcp.defaultPort` | — | `TcpChannel` |
| `TransportConfig` | `Tls` | `tls.{certFile,keyFile,caFile,verifyServer}` | `certFile`/`keyFile` 成对 | `TlsChannel` |
| `TransportConfig` | `Serial` | `serial.{portName,baudRate,...}` | `portName` 非空 | `SerialChannel`（v1） |

```cpp
// src/Core/include/MyProt/Core/Config.hpp

#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

#include "MyProt/Core/Optional.hpp"    // Optional<T> (取代 std::optional)
#include "MyProt/Core/ByteOrder.hpp"   // LengthFieldConfig 依赖 ByteOrder 枚举

namespace MyProt { namespace Core {

// ────────── 帧结构配置 (discriminated union) ──────────

struct LengthFieldConfig {
    int lengthFieldOffset = 0;
    int lengthFieldLength = 0;
    bool lengthIncludesHeader = false;
    ByteOrder byteOrder = ByteOrder::BigEndian;
    int headerLength = 0;
    int lengthAdjustment = 0;           // bodyLen = parsedLen + lengthAdjustment
                                        // (用于协议帧长≠实际数据长度时, e.g. 含 CRC 尾字节)
    int maxFrameSize = 1024;            // 运行时帧长度安全上限; 超出 → 关闭连接 + CircuitOpen
                                         // (协议级安全闸, 防畸形 lengthField 拖垮内存)
};

struct FixedConfig {
    int fixedLength = 0;
};

struct SilenceConfig {               // 串口/RTU 静默成帧 (v1; 成帧由通道读循环驱动)
    int charTimeUs = 0;              // 单字符时间(微秒); 0 = 按协议级波特率+数据位自动折算
    int frameGapUs = 0;              // 帧间静默阈值(微秒); 0 = 3.5 × charTimeUs (RTU 约定)
    int maxFrameSize = 256;
};

struct MessageConfig {               // CAN 消息成帧（未实装；见 ROADMAP.md）
    int maxFrameSize = 8;            // 经典 CAN 8 / CAN FD 64
    int idFieldLength = 2;           // 伪字节流头部 CAN ID 占用字节数
};

enum class FramingType { LengthField, Fixed, Silence, Message };  // 对应 JSON 判别字段 "type"

/// 帧结构配置 — tagged struct (取代 std::variant, ADR-0010 §3);
/// type 决定生效分支, 不得读取未生效分支字段。
struct FramingConfig {
    FramingType type = FramingType::LengthField;
    LengthFieldConfig lengthField;   // type == LengthField 时有效
    FixedConfig fixed;               // type == Fixed 时有效
    SilenceConfig silence;           // type == Silence 时有效
    MessageConfig message;           // type == Message 时有效（v1 不实装）
};

// ────────── 传输配置 ──────────

struct TcpTransportConfig {
    uint16_t defaultPort = 502;         // 与 adl_serializer 缺省值一致 (Modbus TCP)
};

/// TcpTransportConfig 字段边界
///
/// - `defaultPort`：协议级默认端口；`DeviceConfig.connection.port = 0` 时使用。
///   - 取值范围 1~65535；非 0 缺省值仅在协议 JSON 显式声明时被序列化。
///   - 加载期校验：与协议作者文档中"协议默认端口"一致（如 Modbus 502、IEC104 2404、MELSEC 5000）。
///     不一致 → 加载期 **Warning**（不阻断，协议作者可能故意覆盖）。

struct TlsTransportConfig {
    uint16_t defaultPort = 0;
    std::string certFile;
    std::string keyFile;
    std::string caFile;
    bool verifyServer = true;
};

/// TlsTransportConfig 字段边界（v1 stub 状态）
///
/// - `defaultPort`：`Tls` 未实装 → 缺省 0；运行时 TlsChannel 收到非 0 值仍会回 `NotImplemented`（见 [ROADMAP.md](../ROADMAP.md)）。
/// - `certFile` / `keyFile`：PEM 格式证书 + 私钥路径。**成对校验**：必须同时非空或同时为空。
///   仅一者设置 → **ConfigError**。
/// - `caFile`：CA 证书链（可选）。`verifyServer=true` 但 `caFile=""` → 退到系统 trust store。
/// - `verifyServer`：**当前 Tls 为 stub，不生效**（实装后走 OpenSSL 验证）；该字段为配置期契约，
///   见 [ROADMAP.md](../ROADMAP.md)。
///
/// **为什么 Tls 字段保留**：协议作者可预先写"将来切换 TLS"的协议包；字段保留使切换只需把
/// `transport.type` 从 `"Tcp"` 改为 `"Tls"`。

struct SerialTransportConfig {
    std::string portName;    // COM1, /dev/ttyUSB0
    uint32_t baudRate = 9600;
    uint8_t dataBits = 8;
    enum class Parity { None, Odd, Even };
    Parity parity = Parity::None;
    enum class StopBits { One, Two };
    StopBits stopBits = StopBits::One;
};

/// SerialTransportConfig 字段边界与跨平台端口命名
///
/// | 字段         | 取值范围 / 平台约定                                                              | 加载期校验 |
/// |--------------|----------------------------------------------------------------------------------|------------|
/// | `portName`   | Windows: "COM1"..."COM256"（含 USB 串口映射）；Linux: "/dev/ttyUSB0" 等设备节点 | 必填；非空；平台正则匹配；不存在 → ConfigError（启动期尝试 open() 失败捕获） |
/// | `baudRate`   | 110 / 300 / 600 / 1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600 | 必须在常用波特率表内（其他值在部分 USB-Serial 上不支持）|
/// | `dataBits`   | 5 / 6 / 7 / 8                                                                 | 不在 {5,6,7,8} → ConfigError |
/// | `parity`     | None / Odd / Even                                                              | JSON 字符串 "None"/"Odd"/"Even" → 枚举映射（区分大小写）|
/// | `stopBits`   | One / Two                                                                      | JSON 字符串 "One"/"Two" → 枚举映射（不接受 "1.5"） |
///
/// **握手超时**：`device.connection.timeoutMs` 在串口场景下被解读为"打开串口 + 设置参数"的
/// 上限；**不**包含首次请求-应答往返（仍走 `device.requestTimeoutMs`）。
///
/// **Framing 联动**：Serial 默认绑定 `FramingType::Silence`；协议级 `framing` 不显式设置
/// 时，加载器按 transport.type 自动选 Silence 帧格式（无需用户手填）。详见 §1.2 字段取用矩阵。

enum class TransportType { Tcp, Tls, Serial };  // 对应 JSON 判别字段 "type"; CAN 在 v1 不建模（校验器报名称错误）

/// 传输配置 — tagged struct (取代 std::variant, ADR-0010 §3);
/// type 决定生效分支, 不得读取未生效分支字段。
struct TransportConfig {
    TransportType type = TransportType::Tcp;
    TcpTransportConfig tcp;          // type == Tcp 时有效
    TlsTransportConfig tls;          // type == Tls 时有效
    SerialTransportConfig serial;    // type == Serial 时有效
};

/// TransportType → 运行时 Channel 派发（v1 实际状态）
///
/// | TransportType | Channel 实现                | v1 状态 | 失败行为                          |
/// |---------------|-----------------------------|---------|-----------------------------------|
/// | `Tcp`         | `TcpChannel`（asio 1.20.0） | **实装** | 连接失败 → `ConnectionRefused`；读取超时 → `Timeout` |
/// | `Tls`         | `TlsChannel`（OpenSSL 1.1.1） | **v1 stub**（编译期占位，连接时回 `NotImplemented`）| 启动期可达，运行期连接报 `NotImplemented` → HTTP 501 |
/// | `Serial`      | `SerialChannel`（asio serial_port） | **实装** | 串口打开失败 → `ConnectionRefused`；读超时 → `Timeout` |
///
/// **Tls 为何留 stub**：VS2015 + OpenSSL 1.1.1 集成测试覆盖度不足，优先保证 TCP/Serial
/// 双通道稳定；Tls 派发路径已编译（不强删枚举），运行期由 `IChannel::Connect` 显式
/// 返回 `Unexpected(NotImplemented)`，避免静默"使用 TLS 字段但实际走 TCP"的语义错乱。
/// TLS 实装待定（[ADR-0002](../adr/0002-transport-abstraction.md) §6 备选方案延后），见 [ROADMAP.md](../ROADMAP.md)。

// ────────── 响应解析器配置 ──────────

/// 解析器只输出原始字节; 最终类型转换由 tag.finalType 决定 (见 Config_Schema §6)
struct ResponseParserConfig {
    std::string validCondition;        // e.g. "resp[1]==0x03"; 空=跳过校验
    int dataStartIndex = 0;
    std::string dataLengthExpr;        // e.g. "resp[2]"; 空=帧长 - dataStartIndex
};

/// ResponseParser 数据长度裁决优先级（自上而下短路）：
///
/// 1. **`dataLengthExpr` 非空** → 表达式引擎求值（如 `"resp[2]"` 取 1 字节作为长度）。
///    求值结果作为数据区字节数；引擎只取"必要字节"（不大于实际帧长），不足则按读到多少算多少。
/// 2. **`dataLengthExpr` 空** → 取 `帧总长 - dataStartIndex`。
///    即"成帧后剩下的全部字节"；适用于固定头 + 变长 body 的协议（如 S7）。
/// 3. **`dataStartIndex` 越界**（> 实际帧长） → 解析器报 `ParseError`（Bad Quality）。
/// 4. **写路径特殊处理**：`validCondition` 用于 echo 校验（`resp[7] == 0x10`），
///    `dataStartIndex` / `dataLengthExpr` **不解析**——写应答数据区不做语义提取。
///    见 §1.2.1 写操作名约定专项。

// ────────── 操作配置 ──────────

struct OperationConfig {
    std::string name;                  // 由加载器从 operations map 的 key 回填, JSON 中不出现
    std::vector<std::string> requestTemplate;
    ResponseParserConfig responseParser;
    // kind: 操作语义标注 (可选, 空 = 未标注) — "read" | "write".
    //   运行时不依赖 (写路径按引用区分), 用于 UI 表单过滤与配置校验
    //   (标签 operation 应为 read 类, writeOperation 应为 write 类).
    std::string kind;
    // inputs/outputs: 输入/输出参数分组.
    //   inputs : 操作者提供 / 运行时生成的输入 ( source=static 有 value 注入扁平变量池,
    //            无 value 即纯 UI 提示; source=auto 走 autoIncrement/frameSlice/expr/crc ).
    //   outputs: 由输入算出的派生输出 ( source=auto strategy=derivedLength, expr 引用
    //            inputs 已声明名 ∪ 模板 raw 载荷名, 参数层预解析注入变量池, 模板仅查表).
    //   - 同一操作域内 inputs 与 outputs 不得同名 (校验 Error, 防"一个名字串场").
    //   - 跨操作允许角色变化: 同名变量在不同操作可以是 input 或 output
    //     (如 RegisterCount: 读操作=输入 / 写操作=输出), 合法且各自声明可见.
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;
    // 注: 单次请求-应答超时统一由 device.requestTimeoutMs 配置,
    //     协议操作不再持有 timeoutMs 字段, 避免协议/设备两处超时漂移。
    // 注: 写操作复写以"操作名约定"区分(WriteSingleRegister / WriteMultipleRegisters),
    //     不新增 schema 字段, 沿用 responseParser.validCondition 做 echo 校验。
};

/// ────────── 协议变量声明 ──────────

/// 协议变量统一声明 — 一个变量在一处声明其来源 + 值/策略 + 展示元信息.
///
///   source 语义 (仅两种, 其余取值直接报错):
///     "static" — 声明. 含 value → 运行时注入 ctx.variables (标签 variables 同名键覆盖);
///                无 value → 纯展示元信息 (不进 ctx.variables, 仅渲染 UI 提示).
///     "auto"   — 自动计算. 模板 {Name:Xn}/{Name:raw} 消费其"参数层已解析值".
///
///   strategy (source=auto 时):
///     autoIncrement / frameSlice / expr / crc — 调用即自动求值 (拼进 autoComputeJson 喂 AutoComputeProvider)
///     derivedLength                      — 派生长度: 参数层按载荷字节数(payload) 的纯函数预解析,
///       只用 expr (e.g. "{WriteValue:len} + 7" / "{WriteValue:len} * 8").
///
/// JSON 形态(参数分层，计算下沉):
///   "inputs": {
///     "UnitID":        { "source": "static", "value": 1, "label": "从站地址", "enum": [1..10] },
///     "TransactionID": { "source": "auto",   "strategy": "autoIncrement", "params": { "seed": 1 } }
///   },
///   "operations": {
///     "WriteMultipleRegisters": {
///       "inputs":  { "StartAddress": { "source": "static", "label": "起始寄存器地址" },
///                    "WriteValue":   { "source": "static", "label": "写入载荷" } },
///       "outputs": { "PDULength": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7", "label": "PDU 长度" } }
///     }
///   }
struct VariableConfig {
    std::string source;        // "static" | "auto"
    // static 段: 有值 → 运行时注入 ctx.variables; 无值 → 纯 UI 展示
    Optional<uint32_t> value;  // source=static 时可选 (有值才注入)
    // auto 段
    std::string strategy;      // source=auto 时必填: autoIncrement|frameSlice|expr|crc|derivedLength
    std::string paramsJson;    // source=auto 时有效 (derivedLength 不使用): 策略参数 (AutoComputeProvider 消费)
    // source=auto + strategy=derivedLength:
    //   expr: 基于载荷字节数的轻量算术表达式 (e.g. "{WriteValue:len} + 7", "{WriteValue:len} * 8");
    //   {name:len} 引用模板中变量的字节长度 (raw 载荷 = 实际字节数, 其余 = 模板渲染宽度).
    std::string expr;
    // 展示元信息 (所有 source 通用)
    std::string label;         // 显示名 (e.g. "从站地址")
    std::string unit;          // 单位 (e.g. "字节", "Hz", "个")
    // 可选枚举候选 (UI 下拉); JSON 键为 "enum":
    //   元素为裸数字或 { "value": N, "label": "..." } 对象 (Config_Schema §3.2)
    struct EnumMember { uint32_t value; std::string label; };
    std::vector<EnumMember> enumValues;
    std::string placeholder;   // 占位提示 (UI 输入框 placeholder)

    bool isStatic() const { return source == "static"; }
    bool isAuto()   const { return source == "auto"; }
    bool isDerivedLength() const { return isAuto() && strategy == "derivedLength"; }
};

/// §1.2.3 表达式引擎与占位符语法（与 [modules/02_Engine §2.1](./02_Engine.md) 衔接）
///
/// 模板中形如 `{Name}` / `{Name:Xn}` / `{Name:raw}` / `resp[N]` / `resp[a:b]` 的字面量
/// 由 `ExpressionEvaluator`（位于 [modules/02_Engine §2.2](./02_Engine.md)）求值。EBNF 概要：
///
/// ```text
/// expr        = term { ("+" | "-" | "<<" | ">>" | "&" | "|" | "^") term }
/// term        = factor { ("*" | "/" | "%") factor }
/// factor      = number | placeholder | slice | "(" expr ")"
/// placeholder = "{" name [":" format] "}"
/// format      = "X" digit+       // 定宽 hex
///              | "raw"            // 变长字节 (P1 A)
///              | (缺省)           // 默认 hex/ASCII 视 name 类别
/// slice       = "resp[" [int] [":" [int]] "]"
/// name        = identifier (字母/数字/下划线, 首字符非数字)
/// ```
///
/// **占位符类别**：
///
/// | 形态 | 路由入口 | 适用场景 | 示例 |
/// |------|----------|----------|------|
/// | `{Name}` | `Build`（按 `name` 查 `tag.variables` / 协议 inputs 静态值） | 简单数值/字节 | `{RegisterCount}` |
/// | `{Name:Xn}` | `Build`（n 位定宽 hex，左补 0） | 1~8 字节定宽标量 | `{StartAddress:X4}` → 4 字节大端 |
/// | `{Name:raw}` | `BuildBytes`（hex 字符串 → `vector<uint8_t>` 直插） | 变长字节（P1 A）| `{Payload:raw}` |
///
/// **`resp[...]` 切片**：
///
/// - `resp[5]` → 1 字节（按无符号整数返回，0~255）
/// - `resp[5:9]` → 字节切片（4 字节），用于 `sessionExtractExpr` 提取 SessionID
/// - `resp[]` → 整帧（仅供 `validCondition` 使用，不作占位符）
///
/// **优先级**：EBNF 已对齐 C 语言优先级（`* / %` 高于 `+ -` 高于 `<< >>` 高于 `&` 高于 `^` 高于 `|`）。
/// 表达式求值**不**支持函数调用 / 浮点（仅整数算术）；需要复杂计算时由协议作者的 calc 内置
/// 函数在写路径处理（详见 §1.2.4）。
///
/// **求值失败的容错**：
/// - 占位符 `name` 不在变量表中（写操作运行期注入如 `WriteValue` 仍属"运行期注入"，
///   加载期**不**要求存在）→ 运行期回 `BuildError`（Bad Quality，标记请求未出门）。
/// - `resp[...]` 越界（`maxFrameSize` 内但切片越界）→ `ParseError`（Bad Quality）。
/// - hex 字符串非合法字符（"0G" 之类的非 0-9a-fA-F）→ `BuildError`（仅 `{Name:raw}` 路径）。

// ────────── 握手步骤 ──────────

### 1.2.1 写操作名约定（ADR-0007 销账专项）

写操作不新增 schema 字段，靠 `OperationConfig.name` 字面量约定区分。协议作者在 `operations` map 中以约定名声明写操作，`TagReader::WriteOnce` / `WriteBytes` 按名查表：

| 操作名 | 适用 | 变量表自动注入 | responseParser 用法 | 备注 |
|--------|------|----------------|---------------------|------|
| `WriteSingleRegister` | 单寄存器写（FC06） | `StartAddress` (tag), `WriteValue` (标量 uint32) | 仅做 echo 校验（`validCondition`，如 `resp[7] == 0x06`），不解析数据区 | 标量写主入口 |
| `WriteMultipleRegisters` | 多寄存器写（FC16） | `StartAddress`, `RegisterCount`, `RegByteCount`（由载荷长度自动算） | 同上（`validCondition`，如 `resp[7] == 0x10`） | 变长写主入口（`{Name:raw}` 载荷） |

**协议作者契约**：

- 写操作的 `requestTemplate` 仍使用与读操作同一套占位符语法：`{Name:Xn}`（定宽标量）、`{Name:raw}`（变长字节）。
- `responseParser.validCondition` 必须严格做 echo 校验：**不**解析数据区，因写应答数据区通常仅为重复的回显。
- 写操作**不必**在 `tags` 中被任何 `TagDefinition.operation` 引用——`tags` 标记只引用读操作，写操作按操作名直查。配置加载器对"未被引用的写操作"**不**报"未使用"警告（写入协议自洽）。
- 启动期规则 14（Config_Schema §7）只校验 tag 引用的操作；写操作的运行期注入变量（`WriteValue`/`RegisterCount`）**不会**在加载期报"模板变量未定义"。

### 1.2.2 schemaVersion 加载器判定流程（ADR-0005）

`ConfigRoot.schemaVersion` 与 `ProtocolConfig.schemaVersion` 在 `JsonConfigLoader` 加载流程中插入"**版本门禁**"步骤，必须在字段级校验之前完成：

```text
LoadConfig(jsonPath)
  ↓
[1] 解析 JSON → 内存对象 (nlohmann::json)
  ↓
[2] 读取根 schemaVersion 字段
  ↓
[3] 版本门禁 (Version Gate):
    if 缺省 → log("schemaVersion 缺省, 假定=1") → 继续
    elif == kSupportedSchemaVersion(=2) → 继续
    elif <  kSupportedSchemaVersion → return ConfigError("配置版本过旧 ...")
    elif >  kSupportedSchemaVersion → return ConfigError("配置来自更高版本 ...")
  ↓
[4] 对每个 protocols/*.json 重复 [2][3]; 与根不一致 → ConfigError
  ↓
[5] 字段级 ConfigValidator (类型/必填/字段取用矩阵)
  ↓
[6] 产出强类型 POCO (ConfigRoot + 内部 ProtocolConfig/DeviceConfig/TagDefinition)
```

> 门禁在字段校验之前——避免旧配置触发一堆无意义字段错误（ADR-0005 §3）。

### 1.2.4 标签与操作的引用关系矩阵（启动期校验 / 循环引用检测）

`TagDefinition` / `OperationConfig` / `HandshakeStep` 三者存在**非平凡引用关系**，启动期加载器需校验：

| 关系 | 来源 | 目标 | 加载期校验 | 运行时引用 |
|------|------|------|------------|------------|
| tag 引用 op | `TagDefinition.operation` | `operations[name]` | 必须存在；同名 op 必须为读操作（`name` 不以 `Write` 开头） | 调度时按 op.name 查 |
| tag 引用 device | `TagDefinition.deviceId` | `devices[id]` | 必须存在；type 一致（`device.protocol` 必须等于 `operations[op].protocol`） | 调度时按 deviceId 查 |
| handshake 引用 op | `HandshakeStep.requestTemplate` | `operations[name]` (运行期模板) | **不**强校验（handshake 是协议级，op 在协议级内查表） | 每步独立查 |
| 写操作 op 引用 tag | `tag.operation` | — | **无**（写操作不被 tag 引用，仅 WebApi 按 op.name 直查） | 直查表 |
| 表达式 `{Name}` 引用 | 占位符 `name` | `tag.variables` / 协议 inputs 静态值 / handshake 注入 | 加载期仅校验**读**操作的占位符（写操作运行期注入） | 渲染时查 |
| `sessionVariable` 引用 | handshake step | 后续 `requestTemplate` 的占位符 `name` | 加载期必须**前向**可达（Step N 引用 Step M<M 的 session 变量） | 渲染时查 |

**循环引用检测**：

- `TagDefinition` ↔ `OperationConfig` 单向（仅 tag → op），**不可能**循环。
- `OperationConfig.requestTemplate` 内占位符不引用其它 op（仅引用变量），**不可能**循环。
- `HandshakeStep.sessionVariable` 必须**前向**可达（Step N 只能引用 Step M<N 的变量）；
  加载器对反向引用（Step 1 引用 Step 3 变量）报 **ConfigError**。
- 协议作者**不必**手动管理 `handshake` 内 step 间引用；加载器在内存中维护 `session_var_pool`，
  按 step 顺序累积，Step N 渲染时查询 `pool[Step 1..N-1]` 的并集。

**"孤儿操作"判定**（即不被任何 tag 引用的 op）：

- 读操作孤儿 → 加载期 **Warning**（不阻断），提示"配置中存在但未使用的 op"。
- 写操作孤儿 → **不**告警（写操作可独立存在，WebApi 端按 op.name 直查）。
- handshake step 孤儿（不被任何 handshake 数组引用）→ 仅当 `operations` 数量 > handshake 引用 op 数量时
  报告；写操作不被视为孤儿。

**协议 vs 全局 op 重名**：单协议内 op name 必须唯一（`unordered_map` key）；不同协议间
op name 允许重名（`TagDefinition.protocol` 提供命名空间隔离）。

```cpp
struct HandshakeStep {
    std::string name;
    std::vector<std::string> requestTemplate;
    Optional<FramingConfig> framingOverride;        // 该步独立帧格式(JSON null/缺失 = 使用通道默认)
    std::string validCondition;                     // 成功条件表达式
    std::string sessionExtractExpr;                 // 会话变量提取表达式, e.g. "resp[5:9]"
    std::string sessionVariable;                    // 提取后存入的变量名, e.g. "SessionID"
    int timeoutMs = 0;                              // 0 = 继承 device.connection.timeoutMs
};
```

/// HandshakeStep 顺序执行 + 设备生命周期联动（ADR-0010）
///
/// 设备生命周期状态机（5 态，详见 architecture/04 §4.3）：
/// `New → Connecting → Connected → Degraded → Disabled`（自愈失败后退到 Disabled）。
///
/// 握手阶段失败映射：
///
/// | 失败位置 | 状态转移              | 错误码           | 后续行为                              |
/// |----------|-----------------------|------------------|---------------------------------------|
/// | Step 1   | New → Connecting → Disabled | `ConnectionRefused` 或 `Timeout` | 设备被标记 Disabled；不重试（需 SIGHUP/reload 触发重连） |
/// | Step N（N>1，前 N-1 步成功） | Connecting → Connecting（重试）| `Timeout` / `InvalidResponse` | 该设备握手总尝试次数 = 1；总超时 = Σ(step.timeoutMs) |
/// | 全部 Step 成功 | Connecting → Connected | (无) | 设备进入 Connected，可被 TagReader 轮询 |
/// | 握手后首次请求失败 | Connected → Degraded | `Timeout` / `ConnectionClosed` | 进入 Degraded 态，熔断器计数 |
/// | Degraded 累计 `failureThreshold` 次 | Degraded → Disabled | (无) | 设备被标记 Disabled；持久化 + WebApi 触发告警 |
///
/// **握手总尝试次数**：`handshake` 数组执行过程中若**任一**步骤失败 → 整体握手失败（**不**分步重试），
/// 退到 Disabled 态；运营可通过 `POST /api/devices/<id>/reconnect` 触发重新握手。
/// 这避免了"前 3 步成功 + 第 4 步失败"时已分配会话资源（Step 3 提取的 SessionID）但
/// 未提交到请求模板的脏状态。
///
/// **`sessionExtractExpr` 失败的容错**：
/// - 表达式求值失败（`resp[5:9]` 越界）→ 该步回 `InvalidResponse` → 整体握手失败。
/// - 表达式求值成功但 `sessionVariable` 与 `device.username`/`device.password` 重名 →
///   加载期 ConfigError（防止 session 覆盖认证凭据）。
///
/// **`validCondition` 为空**：跳过校验（视为成功）。生产环境**强烈不建议**空——无 echo 校验
/// 等同于"无协议握手"（Modbus 风格的 TCP 直连即如此）。

// ────────── 协议配置 ──────────

struct ProtocolConfig {
    std::string protocolName;
    TransportConfig transport;
    FramingConfig framing;
    Optional<ByteOrder> dataByteOrder;                  // 协议级数据解码字节序; 标签未显式声明时回退至此; 未设置 = BigEndian
    std::unordered_map<std::string, OperationConfig> operations;
    std::vector<HandshakeStep> handshake;             // 空数组 = 无握手 (Modbus)
    int schemaVersion = kSupportedSchemaVersion;      // 配置代际号(ADR-0005); 加载器以 kSupportedSchemaVersion 校验
};

/// 模板函数与校验和裁决
///
/// 模板中的函数型 token（`crc16modbus` / `crc16ccitt` / `crc32` / `lrc` / `xor8`）为**内置能力**：
/// 由校验器按内置白名单核对函数名与格式宽度，协议侧**无需也无处**声明授权。
///
/// 校验和属**协议级**机制（决定帧合法性），不提供"协议作者关闭校验和"的配置路径；
/// 其计算由模板占位符驱动，第二遍扫描在长度字段定型后填充。详见
/// [02_Engine.md](./02_Engine.md) 与 [ADR-0012](../adr/0012-derivedlength-template-primitives-and-trial-render.md)。

// ────────── 设备配置 ──────────

/// 连接参数 (扁平结构, 字段按协议 transport 类型取用; 适用性校验见 Config_Schema §7)
struct ConnectionConfig {
    std::string host;              // Tcp/Tls: 主机名或 IP
    uint16_t port = 0;             // Tcp/Tls: 0 = 使用协议 defaultPort
    std::string portName;          // Serial: 覆盖协议级 portName
    int timeoutMs = 3000;          // 连接建立超时 (与 requestTimeoutMs 严格分离, 见下)
};

/// ConnectionConfig.timeoutMs vs DeviceConfig.requestTimeoutMs — 易混淆边界
///
/// | 字段 | 阶段 | 范围 | 失败映射 |
/// |------|------|------|----------|
/// | `connection.timeoutMs` | **建立 TCP/TLS 串口句柄** | 一次 connect() 调用 | 失败 → `ConnectionRefused`（可重试） |
/// | `device.requestTimeoutMs` | **单次请求-应答往返** | 一次 SendReceive() 调用 | 失败 → `Timeout`（可重试）或 `WriteTimeout`（写不可重试） |
///
/// **两者严格分离**：握手（handshake）走 `connection.timeoutMs`（无独立字段时继承）；
/// 业务读写走 `device.requestTimeoutMs`。`connection.timeoutMs` 与协议握手步骤的
/// `timeoutMs = 0` 互为兜底（handshake 步骤显式 > 0 时优先用步骤值）。
/// 配置加载器对两者**不**做大小关系校验（不同设备策略可能连接慢但响应快，反之亦然）。

/// 韧性策略 (Config_Schema §11 / ADR-0004)。全局块与设备级覆盖同构, 设备级省略字段回退全局值。
struct ResilienceConfig {
    int maxAttempts = 3;           // 读操作总尝试次数(含首次); 写恒为 1
    int backoffBaseMs = 100;       // 指数退避基准
    int backoffMaxMs = 1000;       // 单次退避上限
    int failureThreshold = 5;      // 连续逻辑读取失败(预算耗尽) → 熔断打开
    int cooldownMs = 10000;        // 熔断打开持续时长, 期间 CircuitOpen 快速失败
    int halfOpenProbes = 1;        // 半开态探测次数
};

/// ResilienceConfig 字段回退合并（设备级 vs 全局级）
///
/// 设备级 `DeviceConfig.resilience` 未设置（Optional 为空）→ 用全局 `ConfigRoot.resilience` 全部值。
/// 设备级已设置但**部分字段为 0**（int 缺省值）→ 视为"未设置"，逐字段回退全局。
///
/// | 字段 | 全局默认 | 设备级未设置 → | 设备级设置=0 → | 设备级设置>0 → |
/// |------|----------|----------------|----------------|----------------|
/// | `maxAttempts` | 3 | 用全局 | 用全局 | 用设备级 |
/// | `backoffBaseMs` | 100 | 用全局 | 用全局 | 用设备级 |
/// | `backoffMaxMs` | 1000 | 用全局 | 用全局 | 用设备级 |
/// | `failureThreshold` | 5 | 用全局 | 用全局 | 用设备级 |
/// | `cooldownMs` | 10000 | 用全局 | 用全局 | 用设备级 |
/// | `halfOpenProbes` | 1 | 用全局 | 用全局 | 用设备级 |
///
/// **为什么不用 Optional<int>**：与 `ConnectionConfig` 现有 `int` 字段保持一致，简化 JSON 解析
/// （缺省 = 0 = 缺省路径，无需特别处理"字段不存在" vs "显式为 0"）。
/// **唯一例外**：`maxAttempts` 全局默认 3，但**写操作强制 1**（无论设备级如何设置）；
/// 加载器对设备级 `maxAttempts > 1` 且操作为写名时给出 **Warning**（不阻断），提示用户写重试被忽略。

/// 管理面安全 (Config_Schema §12 / ADR-0008)。ConfigRoot 顶层可选块; nullopt = 全取默认。
struct WebApiConfig {
    std::string bindAddress = "127.0.0.1";  // 监听地址; 默认仅环回, 远程管理需显式改 0.0.0.0
    std::string certFile;                   // TLS 证书; 与 keyFile 齐备即启用 httplib::SSLServer
    std::string keyFile;                    // TLS 私钥; 与 certFile 齐备即启用 TLS
    bool requireAuth = false;               // true 时 token 缺失即启动 Fail-Fast (生产建议 true)
    int rateLimitRps = 5;                   // 敏感端点令牌桶每秒速率
    int rateLimitBurst = 10;                // 令牌桶突发上限, 超限 429
    // 注: token 不入配置文件, 取环境变量 MYPROT_API_TOKEN, 日志脱敏
};

/// WebApiConfig 字段边界与默认行为（呼应 ADR-0008）
///
/// | 字段             | 默认           | 取值范围 / 行为                                              | 加载期校验 |
/// |------------------|----------------|--------------------------------------------------------------|------------|
/// | `bindAddress`    | "127.0.0.1"    | IP 字面量；"0.0.0.0" = 全网卡（需 `requireAuth=true` 兜底）  | 非空；非法 IP → ConfigError |
/// | `certFile`       | ""             | 证书 PEM 路径                                                | 与 `keyFile` 须同时非空 / 同时空；仅一者设置 → ConfigError |
/// | `keyFile`        | ""             | 私钥 PEM 路径                                                | 同上                                                              |
/// | `requireAuth`    | false          | true = 强制 `Authorization: Bearer $MYPROT_API_TOKEN` 校验  | 环境变量缺失 → 启动 Fail-Fast                                      |
/// | `rateLimitRps`   | 5              | 0.1 ~ 1000；0 = 禁用限流（不推荐）                          | < 1 → ConfigError；> 1000 → ConfigError                             |
/// | `rateLimitBurst` | 10             | 1 ~ 100；令牌桶容量                                          | < 1 → ConfigError；> 100 → ConfigError                              |
///
/// **默认 `bindAddress=127.0.0.1` 的目的**：v1 部署默认仅本地管理（防止暴露在公网）；
/// 远程管理必须显式改 `0.0.0.0` + 设置 `requireAuth=true`（详见 ADR-0008 §4 部署场景矩阵）。
///
/// **`rateLimit*` 行为**：
/// - 令牌桶算法：连续 5 RPS 持续允许；突发 10 个后回归稳态 5 RPS。
/// - 超限返回 HTTP 429 Too Many Requests + `Retry-After: 1`。
/// - 限流端点：所有写端点（`POST /api/data/write`）+ 启动期 reload 触发端点；读端点不限流（响应快）。
/// - 限流按来源 IP（`X-Forwarded-For` 信任级别未开启，使用 socket peer）。

struct DeviceConfig {
    std::string id;
    std::string protocol;
    ConnectionConfig connection;           // timeoutMs 仅连接建立
    int requestTimeoutMs = 3000;           // 单次请求-应答超时(ms); 该设备所有操作共用(唯一配置点, 2026-08-24 收敛, ADR-0004 R1)
    Optional<std::string> username;        // 协议级认证凭据, 握手阶段作为模板变量注入, 日志脱敏
    Optional<std::string> password;
    Optional<ResilienceConfig> resilience; // 逐设备韧性覆盖, 未设置 = 用全局 ConfigRoot.resilience
    // 注: 设备级 variableBytesHex 已移除 (v1.32) — 载荷恒由写请求运行时注入
};

// ────────── 标签定义 ──────────

struct TagDefinition {
    std::string name;                   // 全局唯一 (e.g. "PLC-001.Temperature")
    std::string deviceId;
    std::string operation;              // 操作名 (e.g. "ReadHoldingRegisters")
    // 跨协议字节单位 (v1.25/v1.28 收敛): StartByteAddress / ByteCount;
    // 协议族单位 (StartAddress/RegisterCount) 由协议 JSON outputs(derivedLength) 派生
    std::unordered_map<std::string, uint32_t> variables; // { "StartByteAddress": 0, "ByteCount": 2 }
    int scanRateMs = 1000;
    std::string finalType = "UInt16";   // 转换目标类型 (Config_Schema §6)
    Optional<ByteOrder> byteOrder;  // 未设置 = 回退到 协议 dataByteOrder → BigEndian
    bool coalesce = true;               // 是否参与地址邻近合并 (单地址读设 false)
    int bitOffset = -1;                 // 字节内位偏移 0-7 (-1 = 未声明)
    std::string direction = "read";     // "read" | "write" (只写标签, 不参与轮询)
    std::string writeOperation;         // 标量写 (POST value) 操作名; 空 = 不可标量写
    std::string writeBytesOperation;    // 变长写 (POST bytes) 操作名; 空 = 不可变长写
    // 注: deadband / reportMode 已删除 (无消费落点); variableBytesHex 已删除 (v1.32)
    // 注: 请求-应答超时不在此配置, 统一由 device.requestTimeoutMs 决定 (2026-08-24 收敛)
};

// ────────── 配置根 (对应 tags.json) ──────────

struct ConfigRoot {
    int schemaVersion = 1;                       // 配置代际号(ADR-0005); 加载器以 kSupportedSchemaVersion 校验
    Optional<ResilienceConfig> resilience;  // 全局韧性策略 (Config_Schema §11 / ADR-0004); 未设置 = 全取默认
    Optional<WebApiConfig> webApi;          // 管理面安全 (Config_Schema §12 / ADR-0008); 未设置 = 全取默认
    std::vector<DeviceConfig> devices;
    std::vector<TagDefinition> tags;
};

}} // namespace MyProt { namespace Core
```

### JSON 配置格式示例

> 完整 schema（全部字段、默认值、模板文法、校验规则）见 [Config_Schema.md](../Config_Schema.md)。
> 此处仅给出最小可运行片段。**JSON 字段名与上述 POCO 一一对应**。

**tags.json**（节选）：

```jsonc
{
  "devices": [
    {
      "id": "PLC-001",
      "protocol": "modbus-tcp",
      "requestTimeoutMs": 3000,
      "connection": { "host": "192.168.1.100", "port": 502, "timeoutMs": 3000 }
    }
  ],
  "tags": [
    {
      "name": "PLC-001.Temperature",
      "deviceId": "PLC-001",
      "operation": "ReadHoldingRegisters",
      "variables": { "StartAddress": 0, "RegisterCount": 2 },
      "scanRateMs": 5000,
      "registerCount": 2,
      "finalType": "Float",
      "deadband": 0.5,
      "reportMode": "OnChange"
    }
  ]
}
```

**configs/protocols/modbus-tcp.json**（节选）：

```jsonc
{
  "protocolName": "modbus-tcp",
  "transport": { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 4,
    "lengthFieldLength": 2,
    "lengthIncludesHeader": false,
    "byteOrder": "BigEndian",
    "headerLength": 7,
    "lengthAdjustment": 0,
    "maxFrameSize": 260
  },
  "operations": {
    "ReadHoldingRegisters": {
      "requestTemplate": [
        "{TransactionID:auto:X4}",
        "{ProtocolID:X4}",
        "{Length:calc:X4}",
        "{UnitID:X2}",
        "03",
        "{StartAddress:X4}",
        "{RegisterCount:X4}"
      ],
      "responseParser": {
        "validCondition": "resp[7] == 0x03",
        "dataStartIndex": 9,
        "dataLengthExpr": "resp[8]"
      }
    }
  },
  "handshake": [],
  "builtInFunctions": ["auto", "calc"]
}
```

> **占位符说明**（完整文法见 [Config_Schema.md §3](../Config_Schema.md)）：
> - `{Length:calc:X4}` = 引擎两遍扫描时填充"该字段之后到帧尾的字节数"（此例为 6）
> - `{TransactionID:auto:X4}` = 自增事务 ID, 2 字节大端
> - `"03"` = 十六进制字面量（空格分隔字节序列），即功能码
> - 响应数据区 = `resp[9 .. 9+resp[8]]`：MBAP 头(7 字节) + 功能码(1) + 字节数(1) 之后

## 1.3 值类型系统

```cpp
// src/Core/include/MyProt/Core/Value.hpp

#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace MyProt { namespace Core {

enum class QualityCode { Good, Bad, Uncertain };

// Error::Code → QualityCode 映射规则见 §1.3.1 (ADR-0006);
// 各失败分支按该规则直接置值 (无独立辅助函数)。

enum class ValueType {
    Empty,      // 仅用于(Bad/Uncertain 质量)
    ByteArray,
    UInt16, Int16, UInt32, Int32, UInt64, Int64,
    Float, Double,
    Bool,
    String
};

/// 类型化值 — tagged union (取代 std::variant, ADR-0010 §3)。
/// type 决定读取哪个存储字段; 整型统一加宽存放 (UInt16→u, Int16→i), float 加宽到 double (无损)。
struct TypedValue {
    ValueType type = ValueType::Empty;
    std::vector<uint8_t> bytes;   // type == ByteArray
    uint64_t u = 0;               // type == UInt16/UInt32/UInt64
    int64_t i = 0;                // type == Int16/Int32/Int64
    double d = 0.0;               // type == Float/Double
    bool b = false;               // type == Bool
    std::string str;              // type == String
};

struct TagValue {
    std::string tagName;
    std::string deviceId;
    TypedValue typedValue;
    std::vector<uint8_t> rawData;       // Uncertain 时保留原始字节供诊断 (ADR-0006)
    QualityCode quality = QualityCode::Good;  // 失败时按 §1.3.1 规则置值 (ADR-0006)
    Error lastError;
    int64_t timestamp;                // epoch ms
    int64_t requestId;                // 端到端关联 id (correlation id, ADR-0009): 一次逻辑读取全链路共用, 合并请求的所有标签同 id
    bool valueChanged = false;        // 相比上次是否有变化
};

}} // namespace MyProt { namespace Core
```

### 1.3.1 QualityCode 语义与 Error→Quality 映射（ADR-0006）

三值按"设备能否对话 + 数据是否可信"划分（完整裁决见 [ADR-0006](../adr/0006-quality-semantics.md)）：

| 质量码 | 语义 | typedValue | rawData |
|--------|------|-----------|---------|
| `Good` | 完整成功（`validCondition` 通过、转换成功） | 有效值 | 原始数据副本 |
| `Uncertain` | 设备在线且有响应，但本次值不可全信（数据降级） | 通常 `Empty` | **保留原始字节** |
| `Bad` | 无有效数据（通信失败 / 请求未发出 / 帧不可解析 / 配置或内部错误） | `Empty` | 空 |

> **判定原则**：能对话但答非所问 → `Uncertain`；根本没法对话或请求没出门 → `Bad`；一切顺利 → `Good`。

`Error::Code → QualityCode` 的权威映射规则如下：`InvalidResponse` 与 `TypeConversionError` → `Uncertain`（设备在线、有字节返回、但值不可全信），其余错误 → `Bad`，成功路径直接赋 `Good`（各失败分支按此规则置值，不手写例外）。传输类错误在重试窗口内不产出 `TagValue`，预算耗尽后按 `Bad` 产出（衔接 [ADR-0004](../adr/0004-timeout-retry-budget.md)）。

#### 1.3.1.1 QualityCode 边界案例裁决表

易混案例的明确裁决（与上述映射规则一致）：

| 案例 | Error::Code | Quality | 裁决理由 |
|------|-------------|---------|----------|
| 设备回了字节但 `validCondition` 失败 | `InvalidResponse` | `Uncertain` | 设备在线、有响应、但答非所问 |
| 设备回了字节但转换 `finalType` 失败 | `TypeConversionError` | `Uncertain` | 设备在线、有响应、字节合法但语义不达 |
| `validCondition` 通过 + 帧长越界（`maxFrameSize`） | `InvalidResponse` | `Uncertain` | 同上，视为脏应答 |
| 解析中途越界（`dataStartIndex` 越界） | `ParseError` | `Bad` | 协议形态错误，已是 Bad 级 |
| `BuildError`（请求模板渲染失败） | `BuildError` | `Bad` | 请求根本没出门，设备无感知 |
| 写操作 echo 超时（设备没回 echo） | `WriteTimeout` | `Bad` | 写非幂等 → 不可重试 → Bad（与读 Timeout 的可重试语义严格分离） |
| 写操作 echo OK 但 read-back 不一致（P1 B） | `ReadBackMismatch` | `Bad` | 已写入但值不符预期，503 告警 |
| 配置缺失 / 协议未找到（启动期） | `ConfigError` / `ProtocolNotFound` | (无 TagValue 产出) | 启动期 Fail-Fast，不进轮询循环 |
| 熔断器打开 | `CircuitOpen` | `Bad` | 快速失败，预算耗尽后的最后一次回调 |
| 重试窗口内多次 Timeout | (预算耗尽后) `Timeout` | `Bad` | 预算耗尽后按 Bad 产出；窗口内不产出 TagValue |
| 写 `validCondition` 通过（echo OK） | (无 error) | `Good` | 写成功路径直接赋 Good（不写入 typedValue，仅写成功状态） |

> **重要边界**：Quality 裁决**只看 Error::Code**，**不**看调用方主观判断；不允许模块手写 `if (e.code == X) quality = Good`（除成功路径外）。

**占位符 `{Name:raw}` 渲染（变长字节）**：

模板中形如 `{Payload:raw}` 的占位符消费**运行时注入**的载荷字节流：写路径把 `POST /api/data/write` 的 `bytes`（或按 `finalType` 编码的 `value`）以 `writeVariable`（默认 `WriteValue`）为键放入 raw 表，RequestBuilder 在 BuildBytes 阶段把 hex 解析为 `std::vector<uint8_t>` 后直插请求帧。`raw` 是占位符第二段字面量，**不是独立 schema 字段**；配置侧只有 `variables` 一张标量表（v1.32 起 `variableBytesHex` 字段已移除——其声明值从不进入帧）。定宽占位符走 `Build` 入口，变长占位符走 `BuildBytes` 入口，由 RequestBuilder 按占位符第二段路由。

**P1 B read-back 闭环（2026-08-28）**：

`TagReader::WriteOnce` / `WriteBytes` 增 `bool readBack` 参数（默认 false；WebApi 侧由 body 字段 `"readBack": true` 打开）。echo 校验通过后立即对同地址发起读，回读值与下发值不一致回调 `Error::Code::ReadBackMismatch`（`IsRetryable` = false，避免重复写入；按映射规则为 `Bad`；WebApi 侧映射 HTTP 503）。协议无 `ReadHoldingRegisters` 操作时静默跳过 read-back（视为协议不支持，**不**退化为 503）。详见 [modules/05_Gateway.md §5.3](../modules/05_Gateway.md)、[modules/07_WebApi.md §7.5](../modules/07_WebApi.md)、[ADR-0007 §「P1 B read-back 实装」](../adr/0007-write-path-scope.md)。

### 1.3.3 写三态错误的不可重试性（再次确认）

写错误统一不可重试，原因是写非幂等：

| Error::Code | 触发场景 | 重试后果 | 重试策略 |
|-------------|----------|----------|----------|
| `WriteTimeout` | 写操作预算耗尽（设备没回 echo） | 可能设备未收到（不写），也可能已收到（写） | **不重试**，避免双重写入 |
| `WriteFailed` | echo 校验不通过（功能码/异常码错） | 已知写未生效 | **不重试**，由人工排查 |
| `ReadBackMismatch` | echo OK 但写后读回值与下发值不一致 | 已写入（写端确认）但值不符预期 | **不重试**，避免写第二份；503 触发告警 |

`IsRetryable` 对三者均返回 false，**读路径重试逻辑不会触发写操作**。WebApi 层 `POST /api/data/write` 的 `attempt` 永远 = 1（恒单次尝试；超时分支不再计次）。

### 1.3.4 TagValue 全字段流转表

`TagValue` 是数据流的核心载荷，从 `TagReader` 产出 → `PollingEngine::ResultDispatch` 回调直送消费者（LatestValueStore + WebApi `/api/data/latest`，v1 撤回独立 `DataDispatcher` 与死区过滤层，详见 [modules/06_Polling §6.2 撤回横幅](./06_Polling.md#62-datadispatcher-已撤回)）。字段在生命周期内填表规则如下：

| 字段 | 何时填充 | Good 路径 | Uncertain 路径 | Bad 路径 |
|------|----------|-----------|----------------|----------|
| `tagName` | 入口处（TagReader 已知） | ✓ | ✓ | ✓（即便失败也产出 Bad TagValue 供诊断） |
| `deviceId` | 入口处 | ✓ | ✓ | ✓ |
| `typedValue` | 解析后 / 转换后 | 有效值（按 `finalType`） | 通常 `Empty`（保留 typed 字段为初值） | `Empty`（type=Empty 字段归零） |
| `rawData` | 解析后立即填 | 原始数据副本 | **保留原始字节**（核心契约，区别于 Bad） | 空 |
| `quality` | 出口处 | `Good` | `Uncertain`（`InvalidResponse`/`TypeConversionError`） | `Bad`（其余错误 + 成功路径直接赋） |
| `lastError` | 出口处 | `Error{Good, "", ""}`（code 越界哨兵） | 携带 `InvalidResponse`/`TypeConversionError` | 携带 `Timeout`/`ConnectionClosed`/`CircuitOpen` 等 |
| `timestamp` | 解析成功（Uncertain 路径） / 失败回调前（Bad 路径） | 成功时取对端应答时刻 | 同 Uncertain | 失败时取失败回调时刻（贴近对端失败时间） |
| `requestId` | TagReader 入口分配（atomic 计数器，ADR-0009） | 合并请求的所有标签同 id | 同 Uncertain | 同 Bad |
| `valueChanged` | 变化检测（**当前未实现**：生产读路径从不置位，字段恒为 false；`TagDefinition.deadband` / `reportMode` 保留但业务未生效） | 恒为 false | 恒为 false | 恒为 false |

> **消费方契约**：
> - 仅 `Good` 标签的 `typedValue` 可直接当业务值使用。
> - `Uncertain` 标签应保留 `rawData` 用于"参考旧值 + 显示降级标志"策略；不计入 Good 比率指标。
> - `Bad` 标签可丢弃 `typedValue`（空），但**必须保留**以触发告警（计 Bad 指标 + 触发故障日志）。
> - `requestId` 跨链路保持——MQTT/Timescale 持久化时落库该 id，便于回溯一次逻辑读取。

### 1.3.5 TypedValue ↔ finalType 映射表

`tag.finalType` 是配置字符串，[ResponseParser::Parse](./02_Engine.md#25-responseparser--响应解析) 内部按字符串解析并直接调用 `Core::FromBytesU16/U32/U64` 等装配函数填入 `TypedValue.type`（v1 撤回独立 `ConvertToFinalType` 工具函数——撤回 A2，原 [modules/02_Engine §2.7](./02_Engine.md) 整节已删）。字段读取按 `type` 派发：

| `finalType` 字符串 | `TypedValue.type` | 读取字段 | 写入字段 | 字节宽度 |
|-------------------|-------------------|----------|----------|----------|
| `"Bool"` | `ValueType::Bool` | `b` | `b` | 1 |
| `"UInt16"` | `ValueType::UInt16` | `u` | `u` (截断到 16 位) | 2 |
| `"Int16"` | `ValueType::Int16` | `i` (强制 16 位 bit-重解释) | `i` (截断) | 2 |
| `"UInt32"` | `ValueType::UInt32` | `u` (低 32 位) | `u` (截断到 32 位) | 4 |
| `"Int32"` | `ValueType::Int32` | `i` (强制 32 位 bit-重解释) | `i` (截断) | 4 |
| `"UInt64"` | `ValueType::UInt64` | `u` | `u` | 8 |
| `"Int64"` | `ValueType::Int64` | `i` | `i` | 8 |
| `"Float"` | `ValueType::Float` | `d` (float→double 无损) | `d` (写入时 float 截断) | 4 |
| `"Double"` | `ValueType::Double` | `d` | `d` | 8 |
| `"ByteArray"` | `ValueType::ByteArray` | `bytes` | `bytes` | 不定 |
| `"String"` | `ValueType::String` | `str` | `str` | UTF-8 字节数 |
| 其它 / 缺省 | `ValueType::Empty` | (无) | (无) | 0 |

> **位解释约定**：Int16 / Int32 / Int64 在 TypedValue 内部与对应无符号同字段宽度共用（通过 `static_cast` 切换）；写入路径自动 `static_cast<uint32_t>` 防止 C++11 负数转换歧义（呼应 §1.4 末段）。

> **Bool 边界**：`finalType="Bool"` 时 `bytes[0] == 0x00` → `b = false`，非 0 → `b = true`；无字节 → `TypeConversionError`（`Uncertain`）。

> **String 边界**：`finalType="String"` 时按 UTF-8 解码 `bytes`；解码失败 → `TypeConversionError`（`Uncertain`），**不**断字节。响应方契约：WebApi 输出 JSON 时直接 `str` 字段输出（已经是合法 UTF-8）。

### 1.3.6 valueChanged 死区比较的 finalType 限制

`valueChanged` 决定上报策略（`reportMode="OnChange"` 时）：仅在 `|新值 - 上次值| > deadband` 时为 true。

| finalType | deadband 生效? | 比较规则 | deadband=0 行为 |
|-----------|----------------|----------|-----------------|
| `Bool` | ✗ | 恒 `newValue != lastValue` | 同 deadband>0 行为（0 阈值 = 任何变化） |
| `UInt16/Int16/UInt32/Int32/UInt64/Int64` | ✓ | `|new - last| > deadband` | 仅 `!=` 比较（任何数值变化都触发） |
| `Float/Double` | ✓ | `|new - last| > deadband`（双精度比较） | NaN 永不等 → `valueChanged=true`（首字节 NaN 报警） |
| `ByteArray` | ✗ | 逐字节 memcmp | deadband 字段忽略 |
| `String` | ✗ | 字符串 `==` | deadband 字段忽略 |
| `Empty` | ✗ | 恒 false | — |

> **死区缩放规则**：`deadband` 字段是 double，但比较时与同 finalType 字段同等精度（Int 比较时 `static_cast<int64_t>(deadband)`；Bool/String/ByteArray 完全忽略）。`deadband < 0` → 加载期 Warning（不阻断），行为按 0 处理。
>
> **Uncertain 路径的死区比较**：Uncertain 仍与上次 Good 值比较（`rawData` 不参与计算），触发 `valueChanged=true` 时上报（`quality=Uncertain`）——便于上游感知"值变了但可能不准"。`OnChange` 上游应同时感知"值变化 + 质量码"两事件。

### 1.3.7 timestamp 时刻选择与对端时钟信任

`TagValue.timestamp` 始终是**本机 epoch 毫秒**（`std::chrono::system_clock::now()`），**不**取对端设备时间（设备时钟漂移不可信）。

| 阶段 | timestamp 来源 | 用途 |
|------|----------------|------|
| Good 路径 | 应答到达本机的时刻（贴近对端响应时刻） | 上游数据时间线、Timescale 落库 |
| Uncertain 路径 | 解析结束时刻（不取 rawData 中的"对端时间戳字段"） | 同上 |
| Bad 路径 | 失败回调时刻 | 上游告警时间线 |

> **为什么不取对端时间**：协议层不实现 NTP 同步；Modbus / S7 / Melsec 等协议携带的时间戳字段各异（CP37/CP40/DBW0 ...），不抽象。"对端时间"如需作为单独字段，见 [ROADMAP.md](../ROADMAP.md)（`TagValue.remoteTimestamp`）。
>
> **跨主机时钟漂移**：集群部署下，`timestamp` 仅作"事件序列号"使用，**不**作"两台主机数据合并排序"的依据。聚合分析由 Timescale 侧按 `requestId` 关联。

## 1.4 ByteOrder

四序枚举与数值↔线上字节转换，**实现与主机端序无关**（全部位移装配，不依赖本机内存布局，无 `reinterpret_cast`）。

```cpp
// src/Core/include/MyProt/Core/ByteOrder.hpp

namespace MyProt { namespace Core {

enum class ByteOrder {
    BigEndian,          // 标准大端
    LittleEndian,       // 标准小端
    WordBigByteLittle,  // 混合 CDAB: 低字在前, 字内大端 (Melsec)
    WordLittleByteBig   // 混合 BADC: 高字在前, 字内小端
};

// 写路径: 数值 → 线上字节
inline std::vector<uint8_t> ToBytes(uint16_t val, ByteOrder order);
inline std::vector<uint8_t> ToBytes(uint32_t val, ByteOrder order);
inline std::vector<uint8_t> ToBytes(uint64_t val, ByteOrder order);

// 读路径: 线上字节 → 数值(长度不足返回 0)
inline uint16_t FromBytesU16(ByteView data, ByteOrder order);
inline uint32_t FromBytesU32(ByteView data, ByteOrder order);
inline uint64_t FromBytesU64(ByteView data, ByteOrder order);

}} // namespace MyProt { namespace Core
```

**线序语义**（以 u32 = 0x12345678 为例，`wa`=前 2 字节按大端读、`wb`=后 2 字节按大端读）：

| ByteOrder | 线上字节序 | 装配规则 (u32) |
|-----------|-----------|---------------|
| `BigEndian` | `12 34 56 78` | `(wa<<16) \| wb` |
| `LittleEndian` | `78 56 34 12` | 纯小端直接读 |
| `WordBigByteLittle` (CDAB) | `56 78 12 34` | 低字在前、字内大端 → `(wb<<16) \| wa` |
| `WordLittleByteBig` (BADC) | `34 12 78 56` | 高字在前、字内小端, 两字各反转字节序 → `(w1<<16) \| w0` |

> 约定：**16 位单字无"字间序"可言**——除 `LittleEndian` 外一律按大端输出/解析。
> 混合序读写互为逆变换，往返一致；u64 的混合序按两个 32 位字组同理处理。

**u64 混合序展开**（u32 字组 `wa = bytes[0..3] 按字内大端` / `wb = bytes[4..7] 按字内大端`）：

| ByteOrder | u32 线上字节序 | u64 装配规则 |
|-----------|----------------|--------------|
| `BigEndian` | `12 34 56 78` | `(u32hi<<32) \| u32lo`，`u32hi = 0x12345678` |
| `LittleEndian` | `78 56 34 12` | 纯小端直接读 |
| `WordBigByteLittle` (CDAB) | `56 78 12 34` | `(u32lo<<32) \| u32hi`（低 32 位来自前两字节的字组） |
| `WordLittleByteBig` (BADC) | `34 12 78 56` | `(reverse(u32lo)<<32) \| reverse(u32hi)`，高低字内字节序反转 |

> 混合序存在的目的：让"线上一段字节既能被大端协议理解、又能被小端协议理解"——典型是 Melsec（Mitsubishi）协议的浮点 `WordBigByteLittle`、部分 PLC 的 `WordLittleByteBig`。`ToBytes` 与 `FromBytes` 互为逆操作，写完读回值应当完全一致。

**有符号数值**：`ToBytes` 接受 `uint16_t/uint32_t/uint64_t`（无符号）入参，调用方负责把 `int16_t/int32_t/int64_t` 显式 `static_cast` 到无符号（通常 `static_cast<uint32_t>(static_cast<int32_t>(v))` 二步走，先展宽再 bit-重解释）。这是为避免 C++11 中 `int32_t → uint32_t` 在某些路径下出现实现定义（implementation-defined）的负数转换歧义。读取路径返回的 `uint32_t` 需调用方 `static_cast<int32_t>` 回到有符号（bit-重解释，等价于 C++20 `std::bit_cast` 的语义但以 cast 实现）。

#### 1.4.1 ByteView 边界行为（FromBytes 长度不足 / 越界裁剪）

`ByteView` 是 `std::span<const uint8_t>` 的 C++11 替代（避免依赖 C++20 std::span），由 `asio::mutable_buffer` 适配。FromBytes 在长度不足时**返回 0**，**不抛异常**：

| 调用 | 线上字节长度 | 返回值 | 行为 |
|------|--------------|--------|------|
| `FromBytesU16(view, BigEndian)` | ≥ 2 | 解析 | 正常 |
| `FromBytesU16(view, BigEndian)` | 0 或 1 | `0` | **不报错**，按缺省值 |
| `FromBytesU32(view, ...)` | < 4 | `0` | 同上 |
| `FromBytesU64(view, ...)` | < 8 | `0` | 同上 |
| `FromBytesU16(view, ...)` | view 为空（null） | `0` | 不 crash（assert in debug） |

> **为什么不报错**：TagReader 入口已校验 `dataLengthExpr` 范围；FromBytes 仅在"边界之外的最后字节"场景被调用（如帧长 5 字节但表达是 2 字节字段 + 1 字节尾 + 余 2 字节忽略）。返回 0 让上游识别"末尾补 0"是常见协议形态（如 Modbus 多寄存器读末位补 0）。

#### 1.4.2 ToBytes 数值越界裁剪（高位丢弃）

`ToBytes` 在数值宽度超过 `uint*_t` 时**显式截断到 N 字节**，**不**返回错误（与 FromBytes 对称）：

| 调用 | 入参 | 线上字节 | 行为 |
|------|------|----------|------|
| `ToBytes(uint16_t(0x1234), BigEndian)` | 0x1234 | `12 34` | 正常 |
| `ToBytes(uint32_t(0xCAFEBABE), BigEndian)` | 0xCAFEBABE | `CA FE BA BE` | 4 字节 |
| `ToBytes(uint64_t(...), BigEndian)` | 任意 64 位 | 8 字节 | — |
| `ToBytes(uint16_t(0x12345678), BigEndian)` | 超 16 位宽度 | `56 78` | **截断**：高 16 位丢失 |

> **为什么不报错**：与 FromBytes 对称；调用方契约是"按 N 字节宽度准备输入"。`v1` 不做隐式 cast（uint64 → uint16 不会自动窄化），靠 `static_cast<uint16_t>(val)` 显式让编译器给出 truncation warning 提醒。

#### 1.4.3 字节序与主机端序无关性自检

`ToBytes` / `FromBytes` 实现完全用位移装配 + 查表，**不依赖** `htonl` / `ntohl` / `memcpy` 跨类型 reinterp：

```cpp
// 示例: FromBytesU32 (BigEndian)
uint32_t FromBytesU32(ByteView v, ByteOrder o) {
    if (o == ByteOrder::BigEndian) {
        return (uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16)
             | (uint32_t(v[2]) <<  8) |  uint32_t(v[3]);
    }
    // ... 其他序类似
}
```

> **不在大端机上做 htonl 优化**：跨平台行为必须 1:1 一致，**不**留"大端机上有优化路径"的隐式分支。运行时分支由编译器 + 内联优化掉。

---

> **下一节**: [Engine 模块](./02_Engine.md)

---

# 第二部分：横切关注点

> 八个独立契约层，全部位于 `src/Core/include/MyProt/Core/`。**任何模块**引入 Core 都需遵守本部分约束。
> 与第一部分（值类型 + POCO）相对：第二部分描述**机制**，第一部分描述**数据**。

## 1.5 Optional\<T\> — 可选值容器（自写）

> **为什么不用 std::optional**：
> - VS2015 v140 默认 C++14；启用 C++17 需 `/std:c++17`（v140 工具集**不支持**；仅 v141/v142 支持）
> - 第三方 polyfill 引入额外依赖（违反 ADR-0010 零第三方原则）
> - 自写 140 行覆盖 11 文件 30 处 `std::optional` 替换，零依赖、可控、可读
>
> **约束**：T 可默认构造；T 含 `const` 成员时编译错。

```cpp
// src/Core/include/MyProt/Core/Optional.hpp
namespace MyProt { namespace Core {

struct nullopt_t { explicit nullopt_t(int) {} };
const nullopt_t nullopt{0};

namespace detail {
template <typename T>
union OptionalStorage {
    T    _value;
    char _empty;
    OptionalStorage() : _empty() {}
    OptionalStorage(const T& v) : _value(v) {}
    OptionalStorage(T&& v)      : _value(std::move(v)) {}
    ~OptionalStorage() {}
};
} // namespace detail

template <typename T>
class Optional {
public:
    Optional() noexcept : _hasValue(false), _storage() {}
    Optional(nullopt_t) noexcept : _hasValue(false), _storage() {}
    Optional(const T& v) : _hasValue(true), _storage(v) {}
    Optional(T&& v)      : _hasValue(true), _storage(std::move(v)) {}
    Optional(const Optional& o) : _hasValue(o._hasValue) {
        if (_hasValue) ::new (&_storage._value) T(o._storage._value);
    }
    Optional(Optional&& o) noexcept(std::is_nothrow_move_constructible<T>::value)
        : _hasValue(o._hasValue) {
        if (_hasValue) ::new (&_storage._value) T(std::move(o._storage._value));
    }
    ~Optional() { reset(); }
    // ... operator=/swap/emplace/reset/operator->/operator*/value/value_or/has_value/operator bool/==/!=
private:
    bool _hasValue;
    detail::OptionalStorage<T> _storage;  // union storage, 零额外开销
};

} } // namespace MyProt::Core
```

> **API 风格对齐 std::optional**：`has_value()` / `value()` / `value_or(fb)` / `reset()` / `emplace(args...)` / `operator bool` / `operator->` / `operator*` / `operator==` / `operator!=` / `nullopt` / `nullopt_t` — 与 std::optional 行为一致；唯一差异：`value()` 在 debug 中 assert，在 release 不抛异常（项目约定）。

### 1.5.1 Optional API 契约表

| 接口 | 行为 | 失败模式 | 与 std::optional 差异 |
|------|------|----------|---------------------|
| 默认构造 | `_hasValue = false`，`_value = T()` | T 不可默认构造 → 编译错 | 一致 |
| 拷贝构造 | 完整拷贝 | T 不可拷贝 → 编译错 | 一致 |
| 移动构造 | noexcept（依赖 T 移动 noexcept） | 取决于 T | 一致 |
| `has_value()` / `operator bool()` | 显式 bool | — | 一致 |
| `value()` & | assert | debug 中断；release 返回未初始化 | std::optional 抛 `bad_optional_access` |
| `value_or(fallback)` | 有值返回值；无值返 fallback | — | 一致 |
| `operator->` / `operator*` | assert | 同 `value()` | std::optional 不 assert |
| `reset()` | 重置为无值 + `T()` | — | 一致 |
| `emplace(args...)` | 原地构造 | 同上 | 一致 |

### 1.5.2 Fail-Open vs Fail-Close 模式

`Optional<T>` 在 Config POCO 中有两类用途，对应**两种默认行为**：

**Fail-Close（`nullopt` 即拒绝）**：
- 适用：`DeviceConfig.username` / `DeviceConfig.password` / `DeviceConfig.resilience` / `Optional<FramingConfig>`。
- 语义：nullopt = "未配置" = "使用全局默认 / 跳过 / 禁用"。
- 行为：调用方需先 `if (cfg.username)` 再解引用；不解引用 = 走 fallback 路径。

**Fail-Open（`nullopt` 即允许）**：
- 适用：`ResilienceConfig` 字段、设备级 `connection` 覆盖等。
- 语义：nullopt = "未设置" = "使用协议级 / 全局级回退"。
- 行为：加载期按"全局块 + 设备级覆盖"合并（详见 §1.2 ResilienceConfig 表）。

### 1.5.3 与 Expected\<T\> 的语义边界

`Optional<T>` 与 `Expected<T>` 是两个**正交**容器：

| 容器 | 表达 | 何时用 |
|------|------|--------|
| `Optional<T>` | "**值可能不存在**"（如配置项缺省） | 配置 / 默认值 / 缺省值 |
| `Expected<T>` | "操作**可能失败**"（如设备读不到） | 协议层 / I/O / 验证 |

**不可混用**：
- `Optional<Error>` 禁止（既不是"配置缺省"也不是"操作失败"语义）。
- `Expected<Optional<T>>` 允许（罕见：操作成功但可能无值，如"读标签但设备该次未上报"）。

### 1.5.4 性能开销

| 形态 | 体积 | 拷贝成本 |
|------|------|----------|
| `Optional<uint16_t>` | 4 字节（1 bool + 3 padding + uint16_t） | 4 字节 |
| `Optional<std::string>` | 40+ 字节（bool + padding + string 字段） | string 拷贝 |
| `Optional<std::vector<uint8_t>>` | 32 字节 | vector 拷贝（O(N)） |

`vector` / `string` 等大对象在 `Optional` 中仍按值存放（与 `std::optional` 一致）；高频拷贝场景
（如每帧 TagValue 携带 optional）应改为 `Optional<std::shared_ptr<T>>` 共享语义。

### 1.5.5 不可使用场景

1. **T = 引用类型** → T 必须可默认构造（值类型），引用无意义，编译错。
2. **T = `void`** → 改用 `bool has_value_indicator`；不要写 `Optional<void>`。
3. **多态基类 + 值语义切片** → 用 `std::unique_ptr<Base>`（`Optional<Derived>` 切片丢类型）。
4. **T 含 `const` 成员** → 不可默认构造，编译错；改用 mutable 成员或 const-cast。
5. **C++17 `if (auto x = opt)` 不支持**（自写无隐式 bool）→ 必须 `if (opt.has_value())`。
6. **跨线程共享 `Optional<T>`** → 与 `Expected<T>` 同，不安全；调用方负责同步。

---

## 1.6 ByteView — 不可变字节视图（自写，C++11）

> 自写 `ByteView` 取代 `std::span<const uint8_t>`（C++20）/ `gsl::span`（依赖问题）。
> 零拷贝；仅持 `const uint8_t*` + size；**不可拥有**底层内存。

```cpp
// src/Core/include/MyProt/Core/ByteView.hpp
class ByteView {
public:
    ByteView() : _data(nullptr), _size(0) {}
    ByteView(const uint8_t* data, size_t size) : _data(data), _size(size) {}
    ByteView(const std::vector<uint8_t>& v) : _data(v.empty() ? nullptr : v.data()), _size(v.size()) {}

    const uint8_t* data() const { return _data; }
    size_t size() const { return _size; }
    bool empty() const { return _size == 0; }

    const uint8_t& operator[](size_t i) const { assert(i < _size); return _data[i]; }
    const uint8_t& at(size_t i) const { if (i >= _size) throw std::out_of_range("ByteView"); return _data[i]; }

    ByteView subview(size_t offset, size_t len) const {
        assert(offset <= _size);
        size_t actual = (len == npos || offset + len > _size) ? _size - offset : len;
        return ByteView(_data + offset, actual);
    }
    ByteView subview(size_t offset) const { return subview(offset, npos); }

    iterator begin() const { return _data; }
    iterator end() const { return _data + _size; }

    static constexpr size_t npos = static_cast<size_t>(-1);

private:
    const uint8_t* _data;
    size_t _size;
};
```

### 1.6.1 ByteView API 契约表

| 接口 | 行为 | 失败模式 | 与 std::span 差异 |
|------|------|----------|-----------------|
| 默认构造 | `_data=nullptr, _size=0` | — | 一致 |
| 从 `vector<uint8_t>` 构造 | 持 vector 内部指针（**vector 失效时 view 失效**） | — | std::span 同 |
| `data()` / `size()` | 只读访问 | — | 一致 |
| `operator[]` | assert | debug 中断；release 未定义 | std::span 不 assert |
| `at(i)` | **抛 `std::out_of_range`**（与 std::span 一致） | 抛异常 | 一致 |
| `subview(off, len)` | 子视图；`len=npos` 表示到末尾 | assert offset 越界 | 一致 |
| `begin()` / `end()` | 随机访问迭代器 | — | 一致 |

### 1.6.2 生命周期契约（**关键**）

ByteView **不拥有**底层内存；调用方必须保证：

```text
ByteView 的有效期 ⊆ 底层 buffer 的有效期
```

| 持有方式 | 风险 | 缓解 |
|----------|------|------|
| `ByteView` 持 `std::vector<uint8_t>` 内部指针 | vector reallocate / resize 后指针失效 | 仅在 vector **不修改**期间持有；或拷贝为 `std::vector<uint8_t>` |
| `ByteView` 持 `asio::mutable_buffer` | buffer 出 asio scope 失效 | 仅在 asio handler 内持有 |
| `ByteView` 持栈上数组 | 数组退栈后失效 | 禁止跨函数返回；改为 vector |
| `ByteView` 跨线程传递 | 持有方生命周期未对齐 | 加注释或换 `shared_ptr<vector<uint8_t>>` |

### 1.6.3 不可使用场景

1. **跨异步回调持有** → 改用 `shared_ptr<vector<uint8_t>>` 共享所有权。
2. **修改 view 指向的字节** → ByteView 是 `const`；改用 `vector<uint8_t>`。
3. **view 持临时对象** → C++ 临时对象生命周期仅限表达式；C++11 禁止跨语句持有。
4. **C++11 lambda 捕获 by-value** → 捕获的 ByteView 失效时原 vector 可能已被释放。
5. **序列化场景** → 序列化需要拥有；用 `vector<uint8_t>`。
6. **用作 hash key** → 内存地址不稳定（不实现 `std::hash<ByteView>`）。

### 1.6.4 与 vector\<uint8_t\> / std::array 的转换

| 转换方向 | 路径 | 拷贝 |
|----------|------|------|
| `vector` → `ByteView` | 隐式构造 | 零拷贝（持 vector 内部指针） |
| `vector` → `vector` | `view.to_vector()`（**未实现**；手动 `vector<uint8_t>(view.begin(), view.end())`） | O(N) |
| `ByteView` → `vector` | 手动构造 | O(N) |
| `std::array<uint8_t, N>` → `ByteView` | 显式构造 `ByteView(arr.data(), N)` | 零拷贝 |
| `as_bytes(struct)` → `ByteView` | 手动 reinterpret（**禁止**——见 1.6.5） | — |

### 1.6.5 strict-aliasing 风险

**禁止**通过 `ByteView` 跨类型 reinterpret：

```cpp
// ❌ 错误用法
struct Foo { uint32_t a; };
Foo f;
ByteView v(reinterpret_cast<const uint8_t*>(&f), sizeof(f));  // strict-aliasing UB!
```

替代方案：
- 显式逐字段转 bytes（`ToBytes` / `FromBytes` 路径）。
- 实在需要 `memcpy`：`memcpy(buf, &f, sizeof(f))` 后构造 ByteView。

---

## 1.7 日志（自研 ConsoleLogger）

> v1 不引入第三方日志库（撤回 A8 spdlog，vendored 0 引用、依赖 vcpkg 链冗长，改自研轻量 ConsoleLogger）。
> 实现位于 `src/Core/include/MyProt/Core/Log.hpp` + `src/Core/src/Log.cpp`：5 个级别（debug/info/warn/error/critical）+ 线程 id 标签 + 简单花括号格式化（不支持嵌套、字段自动按 `{}` 数量与参数匹配）。
> 4 个全局宏（替代 spdlog 的 6 个宏）：

```cpp
// src/Core/include/MyProt/Core/Log.hpp
#pragma once
#include "MyProt/Core/Log.hpp"

#define MYPROT_LOG_DEBUG(...)   MyProt::Core::Log::Debug(__VA_ARGS__)
#define MYPROT_LOG_INFO(...)    MyProt::Core::Log::Info(__VA_ARGS__)
#define MYPROT_LOG_WARN(...)    MyProt::Core::Log::Warn(__VA_ARGS__)
#define MYPROT_LOG_ERROR(...)   MyProt::Core::Log::Error(__VA_ARGS__)
```

### 1.7.1 5 级别使用边界

| 级别 | 用途 | 频率 | 生产默认 |
|------|------|------|----------|
| `DEBUG` | 调试用详细数据（如解析字节、状态转移）| 高 | **关闭**（默认 INFO） |
| `INFO` | 启动 / 关闭 / 设备状态转移 / 配置 reload | 低 | **开启** |
| `WARN` | 可恢复异常（重试中、读头 1 次失败） | 低 | **开启** |
| `ERROR` | 业务失败（写失败、ReadBackMismatch）| 低 | **开启** |
| `CRITICAL` | 不应发生的内部错误（InternalError / NotImplemented） | 极低 | **开启** |

> **不实装 `TRACE`**：spdlog 6 宏撤回，TRACE 与 DEBUG 合并为 DEBUG；hot-path 性能由"宏在 Debug 构建下不展开"保证（ConsoleLogger::Debug 在 `NDEBUG` 宏下直接 return）。

### 1.7.2 字段脱敏（手动）

撤回 spdlog formatter 自动脱敏——`ConsoleLogger` 不解析花括号字段名，由调用方在 Error::Make 时显式传入 `mask=true` 走脱敏路径：
- 字段名集合：`password` / `passwd` / `secret` / `token` / `apiKey` / `sessionId` / `authToken`。
- `Error::Make(code, message, ctx, /*mask*/true)` 在 `Error::ToString` 阶段把这些字段值替换为 `***`。
- 字符串字面量 `"password=xxx"` **不**自动脱敏（**不推荐**在日志中以字符串拼接形式输出敏感字段，应走 `Error::Make`）。

### 1.7.3 correlationId 显式传递

撤回 spdlog `thread_local` 自动注入——`Log::SetCorrelationId(int64_t reqId)` 改为显式 `Log::Info("[reqId={}] tag={} value={}", reqId, tagName, value)`：

```cpp
void TagReader::OnResponse(int64_t reqId, ...) {
    MYPROT_LOG_INFO("[reqId={}] tag={} value={}", reqId, tagName, value);
}
```

跨线程传递：`requestId` 在 `TagValue` 中显式携带（[ADR-0009](../adr/0009-correlation-tracing.md)）；下游消费方（MQTT/Timescale）按 `requestId` 落库。
**不**使用全局 `thread_local` 跨线程。

### 1.7.4 hot-path 关闭 DEBUG

> **重要**：v1 hot-path（每帧解析、每次 I/O）**禁止** DEBUG 级日志。
> 关闭策略：Release 构建下 `ConsoleLogger::Debug` 编译期 `NDEBUG` 宏直接 return，**零运行时开销**。
> Debug 构建可通过 `Log::SetMinLevel(LogLevel::Info)` 运行时关闭。

### 1.7.5 日志输出策略（默认）

| 通道 | 默认 | 路径 / 端口 | 轮转 |
|------|------|-------------|------|
| 控制台（stdout） | 启动期 + 生产期 | — | 无 |

> **不实装滚动文件/syslog/EventLog**：v1 单一 stdout 通道；接入 systemd journal 或 k8s 容器日志采集即满足生产可观测性；如需本地滚动文件由部署方外部工具（logrotate / 容器 sidecar）接管。ConsoleLogger 内部不维护 sink 注册。

### 1.7.6 不可使用场景

1. **每帧 / 每请求的 INFO** → 关闭或改 DEBUG（hot-path 性能问题）。
2. **日志中输出密码 / token 字面量** → 改用 `Error::Make(..., ctx, /*mask*/true)` 走脱敏路径。
3. **日志中输出整段响应字节** → 改用 RAW 模式（`Log::SetMinLevel(LogLevel::Debug)` + 单独调试输出）；不混入主日志。
4. **跨线程共享 ConsoleLogger 单例** → ConsoleLogger 内部用 mutex 串行化（高频场景应合并多条 INFO 为一条），不阻塞其他业务线程。
5. **日志中 `{}` 数量与参数数量不匹配** → 编译期无 fmt 检查；运行时多余参数截断、不足参数抛 `LogFormatError`（不静默吞错）。

---

## 1.8 ConfigLoader — 三阶段流水线 + 错误聚合

> v1 配置加载器位于 `src/Service/Config/JsonConfigLoader.cpp`。
> 三阶段：**解析 → 版本门禁 → 字段级校验**，每阶段独立错误码。

### 1.8.1 三阶段流水线

```text
LoadConfig(jsonPath)
  ↓
[阶段 1: 解析]
  - 打开文件、读字节
  - nlohmann::json 解析
  - 失败 → ConfigError("JSON 解析失败: ...")
  ↓
[阶段 2: 版本门禁 (ADR-0005)]
  - 读 schemaVersion（根 + 每个 protocols/*.json）
  - 缺省 / 等于 kSupportedSchemaVersion(=2) → 继续
  - < 或 > kSupportedSchemaVersion → ConfigError
  ↓
[阶段 3: 字段级校验]
  - ConfigValidator 逐字段：
    * 类型 / 必填 / 字段取用矩阵
    * 引用关系（§1.2.4 矩阵）
    * 表达式 EBNF 试解析
  - **错误聚合**：不 fail-fast，收集全部错误后一次性报
  - 至少 1 个 Error → ConfigError
  ↓
[阶段 4: 反序列化]
  - 强类型 POCO 产出
  - 回填隐式字段（operation.name、协议/设备级 default 等）
  ↓
[阶段 5: 二次校验]
  - 启动期 ConfigRoot 整体一致性
  - protocol-device 矩阵覆盖（每个 device 必须有 protocol 定义）
  - builtInFunctions 与协议特性匹配（auto 必须有 int 变量）
```

### 1.8.2 错误聚合（不 fail-fast）

阶段 3 中**所有错误聚合**到 `std::vector<ConfigErrorDetail>`，阶段结束一次性报：

```text
配置校验失败（10 个错误）:
  [1] devices[2].connection.host: 必填字段缺失
  [2] devices[3].protocol: 引用的协议 "ModbusTCP-v2" 未在 protocols/ 中定义
  [3] devices[3].requestTimeoutMs: 必须 > 0
  [4] tags[10].operation: 引用的操作 "ReadCoil" 未在 protocols/ModbusTCP/operations 中定义
  [5] tags[12].finalType: 不支持的 finalType "BCD"（支持: Bool/UInt16/...）
  [6] protocols/ModbusTCP.handshake[1].sessionVariable "SessionID" 与 device.password 重名
  [7] protocols/ModbusTCP.operations.WriteCoil: 操作名以 "Write" 开头但 finalType=UInt16 缺失
  [8] 操作 ReadInputRegisters 模板占位符 "{Crc:crc16modbus:X4}" 为已移除的三段文法; 模板仅支持 {Name:Xn} 与 {Name:raw} (规则6)
  [9] protocols/ModbusTCP.framing.type: "Raw" 已移除 (规则5); 迁移表见 Config_Schema §8
  [10] root.resilience.cooldownMs: 必须 > 0
```

**为什么错误聚合**：让用户**一次看到所有配置问题**，避免"改一个错一个"循环。

### 1.8.3 错误码规范

| 阶段 | 错误码 | 严重程度 | 是否阻断 |
|------|--------|----------|----------|
| 解析失败 | `ConfigError` | 致命 | 阻断 |
| 版本门禁失败 | `ConfigError` | 致命 | 阻断 |
| 字段缺失 / 类型错 | `ConfigError` | 致命 | 阻断（聚合后）|
| 字段警告（如 `deadband<0`） | (无 error) | 警告 | **不**阻断（log warn）|
| 孤儿读 op | (无 error) | 警告 | **不**阻断（log warn）|
| 协议字段不匹配文档 | (无 error) | 警告 | **不**阻断（log warn）|
| builtInFunctions 未知 | `ConfigError` | 致命 | 阻断 |

### 1.8.4 hot-reload 约束

- 引擎**不做热替换**：`SIGHUP` 触发**完全重启**。
- reload 触发路径：`SIGHUP` 信号 / `POST /api/admin/reload`。
- 重启流程：保存运行时状态（LatestValueStore）→ 关闭 WebApi → 关闭 PollingEngine → 关闭 ChannelManager → 重新 LoadConfig → 重新初始化。
- reload 期间新连接拒绝；reload 失败回退旧配置（运行时回退见 [ROADMAP.md](../ROADMAP.md)）。

### 1.8.5 不可使用场景

1. **解析时**修改配置 JSON → 加载器捕获 mtime 校验，加载完成后 mtime 变更会触发 reload 提示。
2. **加载完成后**直接改 POCO 字段 → 破坏不可变性契约；reload 时不会重新加载。
3. **跳过阶段 2 门禁** → 配置版本不匹配时仍会触发字段错；门禁必须最先。
4. **加载完成不重置** → reload 时旧 POCO 残留；`ConfigRoot` 必须 RAII。
5. **直接读取 nlohmann::json 节点** → 绕过强类型；必须经 `JsonConfigLoader::ToPOCO`。
6. **多线程并发加载** → 加载器**不**线程安全（应用层保证：仅主线程启动期加载一次）。

---

## 1.9 错误诊断：requestId 关联（ADR-0009）

以 `TagValue.requestId`（`int64_t`）作端到端关联 id：在 `TagReader` 入口分配，同一合并请求的标签**共享同一 id**，随后贯穿「构建 → 发送 → 解析 → 结果分发」全链路（下游消费方按传递值使用，不重分配）。

| 项 | 取值 |
|------|------|
| 分配点 | `TagReader` 入口 |
| 合并请求 | 同组标签共享同一 `requestId` |
| 哨兵值 | `0` = "未进入追踪"，业务代码应过滤 |

**不可使用场景**

1. **跨线程乱用 `requestId`** → 它在 `TagReader` 入口分配，**不**跨线程；下游按传递值使用。
2. **`requestId = 0`** → 哨兵值，业务代码应过滤。

> 分层 span 追踪（`traceId` / `parentSpanId` / W3C Trace Context 跨进程）尚未实装，见 [ROADMAP.md](../ROADMAP.md) 与 [ADR-0009](../adr/0009-correlation-tracing.md)。

---

## 1.10 类型转换矩阵（ResponseParser::Parse 内部）

> 不设独立的类型转换工具函数：类型转换下沉到 [ResponseParser::Parse](./02_Engine.md#25-responseparser--响应解析) 内部按 `byteOrder` + `finalType` 直接调用 `Core::FromBytesU16/U32/U64` 等装配函数。
> 本节定义契约边界。
> 输入：`ByteView`（解析后原始字节）+ `finalType` 字符串 + `byteOrder`。
> 输出：`Expected<TypedValue>`（成功时）+ `Error::Code::TypeConversionError`（失败时）。

### 1.10.1 12 finalType 转换矩阵

| finalType | 期望字节 | 转换规则 | 越界 / 不足行为 | NaN / 非法处理 |
|-----------|----------|----------|------------------|----------------|
| `Bool` | 1 | `bytes[0] != 0` → true | 0 字节 → TypeConversionError | — |
| `UInt16` | 2 | `FromBytesU16(view, byteOrder)` | 不足 2 字节 → 0（隐式）/ TypeConversionError（**显式模式**） | — |
| `Int16` | 2 | `static_cast<int16_t>(FromBytesU16)` | 同上 | — |
| `UInt32` | 4 | `FromBytesU32` | 不足 → 0 / TypeConversionError | — |
| `Int32` | 4 | `static_cast<int32_t>(FromBytesU32)` | 同上 | — |
| `UInt64` | 8 | `FromBytesU64` | 不足 → 0 / TypeConversionError | — |
| `Int64` | 8 | `static_cast<int64_t>(FromBytesU64)` | 同上 | — |
| `Float` | 4 | `FromBytesU32` → `static_cast<float>` | 不足 → 0.0f | NaN / Inf 透传（最终 quality 仍 Good） |
| `Double` | 8 | `FromBytesU64` → `static_cast<double>` | 不足 → 0.0 | NaN / Inf 透传（quality Good） |
| `ByteArray` | 0~N | 整段字节复制 | 空数组合法 | 字节本身合法即可 |
| `String` | 1~N | UTF-8 解码 | 解码失败 → TypeConversionError | — |
| `Empty` | 0 | (无) | 任何字节数合法 | — |

### 1.10.2 隐式 0 vs 显式 TypeConversionError

| 模式 | 触发 | 行为 | 何时启用 |
|------|------|------|----------|
| 隐式 0 | `ByteView` 长度 < 期望字节数 | 静默返回 0 / 0.0 / false | 默认（与 §1.4.1 FromBytes 行为一致）|
| 显式 TypeConversionError | 启动期 `--strict-conversion` flag | 报 `TypeConversionError`（Uncertain Quality）| 生产场景**强烈建议**开启 |

### 1.10.3 NaN / Inf 透传

`finalType=Float/Double` 时不阻断 NaN / Inf，**保留** bit pattern：

```cpp
// v1 实装路径（ResponseParser::Parse 内部）
uint32_t u = FromBytesU32(view, BE);
float f;
memcpy(&f, &u, 4);  // NaN 透传
TypedValue v;
v.d = static_cast<double>(f);  // NaN 仍 NaN
v.type = ValueType::Float;
```

- NaN 在 §1.3.6 deadband 比较中触发 `valueChanged = true`（首字节报警）——但 v1 撤回独立 DataDispatcher 后 deadband 行为 = Always（每轮都透传，详见 [modules/06_Polling §6.2 撤回横幅](./06_Polling.md#62-datadispatcher-已撤回)）；NaN 仍以 Good quality 透传。
- 上游（MQTT）应自行处理 NaN（JSON 中 `null` 或字符串 `"NaN"`）。

### 1.10.4 字节序回退

`tag.byteOrder` 缺省 → 协议 `dataByteOrder` → BigEndian（详见 §1.4）。
`finalType=ByteArray` / `String` 不依赖 byteOrder（按字节序无关处理）。

### 1.10.5 不可使用场景

1. **`finalType=Float` + 字节数 ≠ 4** → TypeConversionError（**不**补 0 当 0.0f）。
2. **`finalType=String` + bytes 非合法 UTF-8** → TypeConversionError（**不**截断到首个非法字节）。
3. **负数数组下标** → 编译期约束（`finalType` 是字符串而非下标）。
4. **ResponseParser::Parse 调用前 bytes 已被移动** → ByteView 失效；拷贝为 vector。
5. **类型转换** hot-path（每帧）→ 性能可接受（O(N) where N ≤ 8）；**不**用 `dynamic_cast`。
6. **依赖 `static_cast` 转换有符号 / 无符号** → 必须经 §1.4 末段两步走 cast 路径。

---

## 1.11 线程安全等级表

> v1 异步模型：**单 `io_context` + 多线程 `run()`** 部署；per-bus strand（P1 D）已撤回——io_context 自身工作窃取已隐式满足"同通道连续提交的 handler 在 worker 线程上按入队顺序执行"，通道内 `strand.post()` 为冗余间接层（详见 [ADR-0002 §6 撤回说明](../adr/0002-transport-abstraction.md#6-撤回说明p1-d--2026-08-29)）。
> 7 个核心 POCO 的线程安全等级如下：

| POCO | 等级 | 备注 |
|------|------|------|
| `Error` | **完全只读** | 构造后字段不变；可自由跨线程共享 |
| `Optional<T>`（自研） | **T 决定** | T 线程安全则 Optional 安全；T 非线程安全则需外层同步（2026-08-29 曾短暂改 `std::optional` 即回退，详见 ADR-0010 §3 二度回退说明） |
| `Expected<T>` | **T 决定** | 同上；`Error` 内嵌，**Error 线程安全** → Expected 整体安全 |
| `ByteView` | **完全只读** | 持 const 指针；不可跨生命周期（见 §1.6.2） |
| `TypedValue` | **构造后只读** | 字段无 mutex；写后转交消费者即释放所有权 |
| `TagValue` | **构造后只读** | 同上；消费方拿到副本后独立 |
| `TagDefinition` | **启动期只读** | 加载后字段不变；多线程读安全 |
| `DeviceConfig` | **启动期只读** | 同上 |
| `ProtocolConfig` | **启动期只读** | 同上 |
| `OperationConfig` | **启动期只读** | 同上 |
| `HandshakeStep` | **启动期只读** | 同上 |
| `WebApiConfig` | **启动期只读** | 同上 |
| `ResilienceConfig` | **启动期只读** | 同上 |

### 1.11.1 可变状态的容器

> 上表之外的**可变状态**结构有显式同步：

| 结构 | 同步机制 | 位置 |
|------|----------|------|
| `LatestValueStore` | `std::shared_mutex`（多读单写） | `src/Polling/src/LatestValueStore.hpp` |
| `ConnectionManager` | `asio::strand`（按 channel 串行） | `src/Gateway/src/ChannelManager.cpp` |
| `CircuitBreaker` | per-device `std::mutex` | `src/Gateway/src/Resilience.cpp` |
| `WebApiServer` | 每个连接独立 strand | `src/WebApi/src/WebApiServer.cpp` |
| `PollingEngine` | 单线程 `io_context` | `src/Polling/src/PollingEngine.cpp` |
| `TagReader._writingInFlight` | per-device `std::atomic<bool>` (P1 C, ADR-0011 §3.1; P1 D 撤回见 ADR-0011 §3.2) | `src/Gateway/src/TagReader.cpp` |

### 1.11.2 "const 是默认契约" 原则

所有 POCO 类**不提供** setter 方法；构造后字段即终态。修改必须构造新对象：

```cpp
// ✓ 正确
TagValue v1 = ...;
TagValue v2 = v1;
v2.valueChanged = true;  // 修改副本

// ❌ 错误（不存在该 API）
v1.setValueChanged(true);
```

### 1.11.3 跨线程传递的两种方式

| 方式 | 适用 | 性能 |
|------|------|------|
| 值传递（拷贝） | 小对象（`Error` / `TagValue` 字段数 ≤ 10） | 栈上拷贝，零分配 |
| `shared_ptr` 共享 | 大对象（`TypedValue.bytes` 大量字节） | 引用计数，原子操作 |

**禁止**裸指针 / 引用跨线程传递（生命周期不明确）。

### 1.11.4 不可使用场景

1. **多线程同时写同一 POCO** → 改用 strand / mutex。
2. **`const_cast` 改 const 字段** → 编译警告；改用 `mutable`（仅限内部状态）。
3. **跨线程传递 ByteView 持 vector 内部指针** → vector 失效；改用 `shared_ptr<vector<uint8_t>>`。
4. **POCO 内部加锁** → POCO 是纯数据；锁放外层容器。
5. **依赖编译期线程安全** → 编译期无法检测数据竞争；靠代码 review + 线程模型文档。

---

## 1.12 配置代际（ADR-0005）

配置以**整数代际号** `schemaVersion` 标识格式版本：`ConfigRoot` 顶层可选携带（拆分布局下写在 `tags.json`），每个协议文件顶层亦可选携带。

当前 `kSupportedSchemaVersion = 2` —— 即 `input` / `outputs` 分组 + `derivedLength` 派生长度的字段集（见 [Config_Schema.md](../Config_Schema.md)）。

**门禁语义**（先于字段校验执行，本清单中唯一会提前终止的检查）：

| 情形 | 结果 |
|------|------|
| 缺省 | Warning + 假定当前代际（`kSupportedSchemaVersion`） |
| 存在但不等于 `kSupportedSchemaVersion` | `ConfigError` Fail-Fast，**不继续字段校验** |
| 协议文件与配置根不一致 | `ConfigError` |

**演进政策**：破坏性格式变更使代际递增。多客户端共存时配置代际**就高不就低** —— 低版本客户端读到更高代际会在门禁处启动失败，须统一升级后再部署。代际迁移工具尚未实装，见 [ROADMAP.md](../ROADMAP.md)。

**不可使用场景**

1. **schemaVersion 与字段不一致**（如低代际配置夹带高代际字段）→ 门禁拒绝。
2. **只升级配置不升级二进制** → 二进制只认自己支持的代际，须同时升级。
3. **回滚时不带 `.bak`** → 配置丢失；建议配置 JSON 纳入版本控制。

详见 [ADR-0005](../adr/0005-config-versioning.md)。

---

## 1.13 本节小结

> 8 个横切关注点（§1.5~1.12）覆盖了 Core 模块的全部"机制层"契约。
> 与第一部分（§1.1~1.4 数据层）合并后，01_Core.md 描述了：
>
> | 层 | 节 | 内容 |
> |----|----|------|
> | 数据层 | §1.1~1.4 | Error / Expected / Optional / Config POCO / TypedValue / TagValue / ByteOrder |
> | 机制层 | §1.5~1.12 | Optional 契约 / ByteView 契约 / 日志 / ConfigLoader / Trace / 类型转换 / 并发 / 版本 |
>
> **跨文档引用**：
> - Engine（02_Engine.md）实现 `ResponseParser::Parse`（含 `FromBytesU16/U32/U64` 装配，v1 撤回 A2 独立 `ConvertToFinalType` 工具函数）/ `ExpressionEvaluator` / `RequestBuilder`
> - Transport（03_Transport.md）实现 `IChannel` 派发
> - Gateway（05_Gateway.md）实现 `TagReader` / 写路径
> - WebApi（07_WebApi.md）实现 `Error → HTTP` 映射
> - ADR-0005/0009 在 §1.8 / §1.9 衔接
> - architecture/04（错误关闭安全）衔接 §1.7 + §1.11

---

## 1.14 关联索引

- **尚未实装的扩展项**（TLS / CAN / W3C Trace / RBAC / 协程化 等）：[ROADMAP.md](../ROADMAP.md)
- **架构决策与理由**：[adr/](../adr/)
- **配置契约唯一事实来源**：[Config_Schema.md](../Config_Schema.md)

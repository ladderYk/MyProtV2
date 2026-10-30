# MyProtV2 — Core Module

> **Series**: MyProtV2 module design
> **Next**: [Engine module](./02_Engine.md)

---

**Positioning**: an INTERFACE library — header-only, **pure C++11**, zero third-party dependencies. Error handling comes from the hand-written `Expected.hpp`; the optional-value container from `Optional.hpp` (a brief switch to `std::optional` on 2026-08-29 was reverted, see [ADR-0010 §3](../../adr/0010-vs2015-cpp11-toolchain.md) *zh*); the bytes view from `ByteView.hpp` (ADR-0010 §3).

> Warning: [Config_Schema.md](../Config_Schema.md) is the single source of truth for configuration POCOs. Where this document conflicts with the schema, the schema wins.

## 1.1 Expected\<T\> (hand-written, C++11)

Hand-written `Expected<T>` performs monadic error handling. The original design used `tl::expected` and was dropped because of VS2015 compile risk (ADR-0010 §3/§4). This implementation is header-only and dependency-free; the value and the error sit side by side (contract: **T must be default-constructible**, which every T in the project satisfies), avoiding union/aligned_storage corner cases on old compilers.

```cpp
// src/Core/include/MyProt/Core/Expected.hpp

#pragma once
#include <cassert>
#include <string>
#include <utility>

namespace MyProt { namespace Core {

/// Error type — the project-wide Single Source of Truth.
/// architecture/04 only references this definition in annotation tables.
struct Error {
    enum class Code {
        // ──── Retryable + idempotent-safe (only read operations retry, see IsRetryable) ────
        Timeout,              // network/device timeout — back off and retry
        ConnectionRefused,    // connection refused — wait for reconnect
        ConnectionClosed,     // connection dropped — auto reconnect
        Busy,                 // device busy — retry later

        // ──── Not retryable (marks Bad Quality) ────
        InvalidResponse,      // response violates validCondition
        ParseError,           // response parsing failed
        BuildError,           // request build failed (fail-fast, logged)
        TypeConversionError,  // raw bytes -> finalType conversion failed

        // ──── Write errors (not retryable; writes are not idempotent) ────
        WriteTimeout,         // write timeout — no retry, avoid double write
        WriteFailed,          // write failed — echo validation failed; no retry
        ReadBackMismatch,     // echo OK but read-back value differs — no retry, HTTP 503
                              //  (P1 B read-back, 2026-08-28; ADR-0007 closed; ADR-0006 -> Bad)

        // ──── Configuration errors (fail-fast at startup) ────
        ConfigError,          // JSON parse/validation failure
        DeviceNotFound,       // device config missing
        TagNotFound,          // tag not found
        ProtocolNotFound,     // protocol name unresolved
        CircuitOpen,          // circuit breaker open — reject request

        // ──── Internal errors ────
        InternalError,        // should never happen
        NotImplemented        // unimplemented feature (only TlsChannel stub uses it today)
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

/// Retryability: reads only (Timeout/ConnectionRefused/ConnectionClosed/Busy).
/// All three write states are non-retryable because writes are not idempotent.
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
```

**Error categories** — `ErrorCategoryName(Error::Code)` returns one of four families:

| Family | Codes |
|---|---|
| `RetryableTransport` | `Timeout` / `ConnectionRefused` / `ConnectionClosed` / `Busy` |
| `DataShapeError` | `InvalidResponse` / `ParseError` / `BuildError` / `TypeConversionError` |
| `WriteError` | `WriteTimeout` / `WriteFailed` / `ReadBackMismatch` |
| `LookupOrStateError` | `ConfigError` / `DeviceNotFound` / `TagNotFound` / `ProtocolNotFound` / `CircuitOpen` |
| `InternalError` | `InternalError` / `NotImplemented` |

**`Error::Code` -> HTTP mapping** (WebApi layer, see [07_WebApi §7.5](./07_WebApi.md)):

| Error::Code | HTTP | Note |
|---|---|---|
| `ConfigError` | 500 | startup fail-fast, should not occur at runtime |
| `DeviceNotFound` | 404 | URL `deviceId` does not exist |
| `TagNotFound` | 404 | `body.tagName` not in configuration |
| `ProtocolNotFound` | 500 | missing config (should have been caught at startup) |
| `CircuitOpen` | 503 | `Retry-After: cooldownMs` |
| `Timeout` | 504 | upstream device timeout (read) |
| `ConnectionRefused` | 502 | upstream connection refused (runtime burst) |
| `ConnectionClosed` | 502 | upstream connection dropped |
| `Busy` | 503 | `Retry-After: backoff` hint |
| `InvalidResponse` | 502 | upstream returned garbage |
| `ParseError` | 502 | protocol parse failure |
| `BuildError` | 500 | internal build error (treated as InternalError) |
| `TypeConversionError` | 502 | finalType conversion failed |
| `WriteTimeout` | 504 | write timeout |
| `WriteFailed` | 502 | echo validation failed |
| `ReadBackMismatch` | 503 | P1 B read-back mismatch (no retry; 503 raises an alert) |
| `InternalError` | 500 | should never happen; pages on-call |
| `NotImplemented` | 501 | not implemented in v1 (TlsChannel stub) |

```cpp
/// Unexpected marker — implicitly constructs any Expected<T> carrying an Error
struct UnexpectedType { Error error; };

inline UnexpectedType Unexpected(Error::Code code,
                                 const std::string& msg = "",
                                 const std::string& ctx = "") {
    UnexpectedType u;
    u.error = Error::Make(code, msg, ctx);
    return u;
}
```

**`Error::context` injection and masking rules**

- Format: comma-separated `key=value` list; no newlines or control characters (prevents log injection).
- Length cap: 256 bytes (truncated beyond that, to bound log volume).
- **Masked field names** (must be filtered before writing into `context`): `password`, `passwd`, `secret`, `token`, `apiKey`, `sessionId` -> replace the value with `***` and keep the key out of `message` as well.
- Protocol-level `username` / `password` are injected as template variables during handshake and **never** enter `context`.
- `Error::SanitizeContext` is an internal helper invoked automatically from `Error::Make`; callers constructing `Error` directly must sanitize themselves.

```cpp
/// Hand-written Expected (replaces tl::expected / std::expected; ADR-0010 §3).
/// Constraint: T is default-constructible.
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

    // ── monadic combinators (C++11; lambda parameter types must be explicit) ──

    /// F: (T&) -> Expected<U>; short-circuits the error
    template <typename F>
    auto and_then(F f) -> decltype(f(std::declval<T&>())) {
        if (_hasValue) { return f(_value); }
        return UnexpectedType{_error};
    }

    /// F: (T&) -> T; transforms the value, short-circuits the error
    template <typename F>
    Expected<T> map(F f) {
        if (_hasValue) { return Expected<T>(f(_value)); }
        return UnexpectedType{_error};
    }

    /// Substitutes a degraded value on error
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

using VoidExpected = Expected<void>;

}} // namespace MyProt { namespace Core
```

**Contract vs `tl::expected` / `std::expected`**

| Interface | This implementation | Difference |
|---|---|---|
| Implicit construct from T | yes (`const T&` / `T&&`) | same |
| Implicit construct from error | yes (`UnexpectedType`) | one extra wrapper avoids overload ambiguity |
| `has_value()` | yes | same |
| `operator bool()` | yes, **always explicit** | prevents accidental `if (e)` misuse |
| `value()` | `assert` (no throw) | zero cost; project does not use exceptions |
| `error()` | `assert` | same |
| `value_or` | yes | same |
| `*` / `->` access | **not implemented** | callers must use `.value()` to avoid reading the inactive branch |
| `and_then` / `map` / `or_else` | yes (hand-written) | lambdas must spell out parameter types (no generic lambdas) |
| T not default-constructible | compile error | hard constraint |

**Not-usable scenarios**

1. `T` is a reference type -> cannot be default-constructed.
2. `T` has a `const` member -> conflicts with the default-constructibility requirement.
3. Large `T` on frequent failure paths -> stored by value, no small-buffer optimisation.
4. C++17 `if (e)` does not apply because the bool conversion is explicit -> use `if (e.has_value())` or `if (!!e)`.
5. Sharing an `Expected` across threads -> unsafe (copying `T` needs synchronisation).

```cpp
// Recommended: explicit check (clear control flow, VS2015 friendly)
Expected<std::vector<uint8_t>> resp = /* SendReceive result */;
if (!resp) { /* resp.error().code / message / context */ return; }
use(resp.value());

// Monadic chaining (explicit lambda parameter types)
Expected<int> result = ParseResponse(data)
    .and_then([](std::vector<uint8_t>& bytes) { return ConvertType(bytes); })
    .map([](int& v) { return v * 2; })
    .or_else(-1);
```

## 1.1.1 Optional\<T\> and ByteView (hand-written)

> `Core::Optional<T>` is home-grown (VS2015 / C++11 has no `std::optional`, see [ADR-0010](../../adr/0010-vs2015-cpp11-toolchain.md) *zh*). `ByteView` also stays hand-written (zero dependencies; a stable replacement for `std::span`).

Both are header-only and dependency-free (ADR-0010 §3). The first-generation implementation keeps the value inline:

```cpp
// src/Core/include/MyProt/Core/Optional.hpp
template <typename T>
class Optional {
public:
    Optional() : _hasValue(false) {}
    Optional(const T& v) : _hasValue(true), _value(v) {}
    Optional(T&& v) : _hasValue(true), _value(std::move(v)) {}

    // Copy/move are spelled out explicitly: T's assignment operator would
    // otherwise suppress the implicit generation and break whole-struct copies.
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
    T _value;   // valid when _hasValue
};
```

```cpp
// src/Core/include/MyProt/Core/ByteView.hpp
using Bytes = std::vector<uint8_t>;

/// Lightweight bytes view (replaces std::span<const uint8_t>; ADR-0010 §3).
/// Owns nothing: the caller guarantees the buffer outlives the view.
struct ByteView {
    const uint8_t* data;
    size_t size;

    ByteView() : data(0), size(0) {}
    ByteView(const uint8_t* d, size_t n) : data(d), size(n) {}
    ByteView(const Bytes& b) : data(b.data()), size(b.size()) {}

    bool empty() const { return size == 0; }
    const uint8_t& operator[](size_t i) const { return data[i]; }
};
```

A second, union-based `Optional` implementation is documented in §1.5; the project keeps both variants in the same header and the union version exposes the `std::optional`-shaped API.

## 1.2 Config POCO — tagged structs (C++11)

> Discriminated unions (`framing` / `transport`) are expressed as **tagged structs**: a top-level `type` enum mirrors the JSON discriminator, all branch fields are laid out flat, and only the active branch may be read. This replaces `std::variant` (ADR-0010 §3).

**JSON <-> tagged struct one-to-one example** (`FramingConfig`):

```json
// config file: configs/protocols/modbus-tcp.json
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
// After deserialization (pseudo):
FramingConfig f;
f.type = FramingType::LengthField;          // <- from "type":"LengthField"
f.lengthField.lengthFieldOffset = 4;
f.lengthField.lengthFieldLength = 2;
f.lengthField.lengthIncludesHeader = false;
f.lengthField.byteOrder = ByteOrder::BigEndian;
f.lengthField.headerLength = 6;
f.lengthField.lengthAdjustment = 0;
f.lengthField.maxFrameSize = 1024;
// Note: f.fixed / f.silence / f.message keep their defaults and are never read
```

**Deserialization discipline**

- `type` string -> enum goes through `FramingTypeParse` / `TransportTypeParse` (unknown values are rejected).
- Every sibling field is deserialized regardless of the branch; at runtime only the active branch's fields are read.
- Unknown fields (e.g. `"futureField": 1`) produce a loader **Warning** (non-blocking) so future extensions are visible.

**Reading constraints for discriminated unions (v1 hard rule)**

- `FramingConfig::type` and `TransportConfig::type` are the **only** runtime discriminator; never infer the branch from "is this field non-zero" (many fields default to 0, so "unset" and "explicitly 0" are indistinguishable).
- Reading an inactive branch is undefined behaviour (C++11 has no `std::variant`-style hold-alternative check). `ConfigValidator` enforces required fields per `type`; runtime code (`TagReader`, `IChannel` construction) dispatches purely on `type`.
- **Runtime fail-safe**: `TagReader` re-checks `op.framing.type` on entry (e.g. `framing == FramingType::Message` yields `ConfigError` because v1 does not implement CAN) and stops that operation before it enters the polling loop.
- **Extension rule**: adding a `framing` branch requires touching four places — the `FramingType` enum, the flat `FramingConfig` fields, `ConfigValidator` required-field checks, and the `TagReader` dispatch. Missing one fails at compile time or runtime.

**Field usage matrix** (`type` -> fields consumed):

| Config | type | Active fields | Load-time validation | Runtime consumer |
|---|---|---|---|---|
| `FramingConfig` | `LengthField` | `lengthField.*` | `lengthFieldLength` in {1,2,4} | `TcpChannel` / `TlsChannel` |
| `FramingConfig` | `Fixed` | `fixed.fixedLength > 0` | `fixedLength` must be > 0 | same |
| `FramingConfig` | `Silence` | `silence.*` | `frameGapUs > 0`, or allow `0 = 3.5x charTimeUs` | `SerialChannel` (v1) |
| `FramingConfig` | `Message` | `message.*` | validator reports name-reserved (CAN not implemented) | see [ROADMAP](../../ROADMAP.md) *zh* |
| `TransportConfig` | `Tcp` | `tcp.defaultPort` | — | `TcpChannel` |
| `TransportConfig` | `Tls` | `tls.{certFile,keyFile,caFile,verifyServer}` | `certFile` / `keyFile` must be paired | `TlsChannel` |
| `TransportConfig` | `Serial` | `serial.{portName,baudRate,...}` | `portName` non-empty | `SerialChannel` (v1) |

```cpp
// src/Core/include/MyProt/Core/Config.hpp

// ────────── framing configuration (discriminated union) ──────────

struct LengthFieldConfig {
    int lengthFieldOffset = 0;
    int lengthFieldLength = 0;
    bool lengthIncludesHeader = false;
    ByteOrder byteOrder = ByteOrder::BigEndian;
    int headerLength = 0;
    int lengthAdjustment = 0;           // bodyLen = parsedLen + lengthAdjustment
                                        // (frames whose length excludes a CRC trailer)
    int maxFrameSize = 1024;            // runtime safety cap; exceeded -> close + CircuitOpen
};

struct FixedConfig {
    int fixedLength = 0;
};

struct SilenceConfig {               // serial/RTU silence framing (v1; driven by the channel read loop)
    int charTimeUs = 0;              // 0 = derived from protocol baudRate + dataBits
    int frameGapUs = 0;              // 0 = 3.5 x charTimeUs (RTU convention)
    int maxFrameSize = 256;
};

struct MessageConfig {               // CAN framing (not implemented; see ROADMAP)
    int maxFrameSize = 8;            // classic CAN 8 / CAN FD 64
    int idFieldLength = 2;           // CAN ID bytes at the head of the pseudo byte stream
};

enum class FramingType { LengthField, Fixed, Silence, Message };

struct FramingConfig {
    FramingType type = FramingType::LengthField;
    LengthFieldConfig lengthField;   // valid when type == LengthField
    FixedConfig fixed;               // valid when type == Fixed
    SilenceConfig silence;           // valid when type == Silence
    MessageConfig message;           // valid when type == Message (not in v1)
};

// ────────── transport configuration ──────────

struct TcpTransportConfig {
    uint16_t defaultPort = 502;      // matches the Modbus TCP default
};
// defaultPort is used when DeviceConfig.connection.port == 0.
// Non-zero defaults are only serialized when the protocol JSON declares them.
// A mismatch with the protocol's documented default (Modbus 502, IEC104 2404,
// MELSEC 5000) is a load-time Warning, not an error.

struct TlsTransportConfig {
    uint16_t defaultPort = 0;
    std::string certFile;            // PEM certificate + private key, must be set as a pair
    std::string keyFile;             // one of the two set -> ConfigError
    std::string caFile;              // optional CA chain; empty = system trust store
    bool verifyServer = true;        // stub in v1, contract only
};
// Tls fields are retained so protocol authors can pre-write a TLS-capable package:
// switching later only flips transport.type from "Tcp" to "Tls".

struct SerialTransportConfig {
    std::string portName;    // COM1, /dev/ttyUSB0
    uint32_t baudRate = 9600;
    uint8_t dataBits = 8;
    enum class Parity { None, Odd, Even };
    Parity parity = Parity::None;
    enum class StopBits { One, Two };
    StopBits stopBits = StopBits::One;
};
```

| Serial field | Range / platform convention | Load-time validation |
|---|---|---|
| `portName` | Windows `COM1`..`COM256` (incl. USB serial); Linux `/dev/ttyUSB0` and friends | required, non-empty, platform regex; open() failure at startup -> `ConfigError` |
| `baudRate` | 110 / 300 / 600 / 1200 / 2400 / 4800 / 9600 / 19200 / 38400 / 57600 / 115200 / 230400 / 460800 / 921600 | must be in the common table |
| `dataBits` | 5 / 6 / 7 / 8 | outside that set -> `ConfigError` |
| `parity` | `None` / `Odd` / `Even` | case-sensitive string -> enum |
| `stopBits` | `One` / `Two` | `"1.5"` is rejected |

> Handshake timeout: in serial scenarios `device.connection.timeoutMs` bounds "open port + apply parameters" only; the first request/response round trip still uses `device.requestTimeoutMs`.
> Framing coupling: `Serial` defaults to `FramingType::Silence`; when the protocol does not set `framing` explicitly the loader picks Silence from `transport.type` automatically.

```cpp
enum class TransportType { Tcp, Tls, Serial };  // CAN is not modelled in v1

struct TransportConfig {
    TransportType type = TransportType::Tcp;
    TcpTransportConfig tcp;          // valid when type == Tcp
    TlsTransportConfig tls;          // valid when type == Tls
    SerialTransportConfig serial;    // valid when type == Serial
};
```

| TransportType | Channel | v1 status | Failure behaviour |
|---|---|---|---|
| `Tcp` | `TcpChannel` (asio 1.20.0) | **implemented** | connect failure -> `ConnectionRefused`; read timeout -> `Timeout` |
| `Tls` | `TlsChannel` (OpenSSL 1.1.1) | **v1 stub** | `IChannel::Connect` returns `NotImplemented` -> HTTP 501 |
| `Serial` | `SerialChannel` (asio serial_port) | **implemented** | open failure -> `ConnectionRefused`; read timeout -> `Timeout` |

> Why TLS stays a stub: VS2015 + OpenSSL 1.1.1 integration coverage is thin, and TCP/Serial stability comes first. The dispatch path is compiled in, and the stub fails loudly instead of silently "using TLS fields but talking plain TCP" ([ADR-0002](../../adr/0002-transport-abstraction.md) *zh* §6, [ROADMAP](../../ROADMAP.md) *zh*).

```cpp
// ────────── response parser configuration ──────────

/// The parser only emits raw bytes; final type conversion belongs to tag.finalType.
struct ResponseParserConfig {
    std::string validCondition;        // e.g. "resp[1]==0x03"; empty = skip validation
    int dataStartIndex = 0;
    std::string dataLengthExpr;        // e.g. "resp[2]"; empty = frame length - dataStartIndex
};
```

**Data-length precedence** (top-down short circuit):

1. `dataLengthExpr` non-empty -> evaluate the expression (e.g. `"resp[2]"` reads one byte as the length). The engine only consumes as many bytes as the frame actually holds.
2. `dataLengthExpr` empty -> `frame length - dataStartIndex` ("everything after the header"), suitable for fixed-header + variable-body protocols such as S7.
3. `dataStartIndex` out of range (> frame length) -> `ParseError` (Bad Quality).
4. Write path: `validCondition` performs echo validation (`resp[7] == 0x10`); `dataStartIndex` / `dataLengthExpr` are **not** parsed because write responses carry no semantic payload.

```cpp
// ────────── operation configuration ──────────

struct OperationConfig {
    std::string name;                  // back-filled by the loader from the operations map key
    std::vector<std::string> requestTemplate;
    ResponseParserConfig responseParser;
    // kind: optional semantic annotation ("read" | "write"), used by UI filtering and
    //   config validation; the runtime distinguishes writes by reference.
    std::string kind;
    // expectResponse: false = fire-and-forget (one-way send). Send completion counts as
    //   success; no read, no framing, no wait. Only write operations may use it.
    //   If the device answers anyway, residual bytes pollute the next request (ADR-0018).
    bool expectResponse;

    // inputs : operator-provided or runtime-generated inputs.
    //   source=static with value -> injected into the flat variable pool;
    //   source=static without value -> pure UI hint;
    //   source=auto -> autoIncrement / frameSlice / expr / crc.
    // outputs: derived outputs computed from inputs (source=auto, strategy=derivedLength,
    //   expr references declared inputs plus template raw payload names).
    //   - inputs and outputs must not share a name inside one operation.
    //   - Across operations the role may change (RegisterCount is an input for reads
    //     and an output for writes).
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;
    // Notes: the request/response timeout lives on device.requestTimeoutMs only;
    //        write operations are identified by the operation-name convention and
    //        reuse responseParser.validCondition for echo validation.

    OperationConfig() : expectResponse(true) {}
};

struct VariableConfig {
    std::string source;        // "static" | "auto"
    // static section: with a value -> runtime injection; without -> UI metadata only
    Optional<uint32_t> value;
    // auto section
    std::string strategy;      // autoIncrement|frameSlice|expr|crc|derivedLength
    std::string paramsJson;    // strategy parameters (not used by derivedLength)
    // source=auto + strategy=derivedLength:
    //   expr: light arithmetic over payload byte counts,
    //   e.g. "{WriteValue:len} + 7" / "{WriteValue:len} * 8".
    std::string expr;
    // display metadata (all sources)
    std::string label;
    std::string unit;
    struct EnumMember { uint32_t value; std::string label; };
    std::vector<EnumMember> enumValues;   // JSON key "enum": bare numbers or {value,label}
    std::string placeholder;

    bool isStatic() const { return source == "static"; }
    bool isAuto()   const { return source == "auto"; }
    bool isDerivedLength() const { return isAuto() && strategy == "derivedLength"; }
};
```

**Expression engine and placeholder grammar** (full description in [02_Engine §2.1-2.2](./02_Engine.md))

Placeholders `{Name}` / `{Name:Xn}` / `{Name:raw}` are expanded by `Engine::RequestBuilder`; `resp[N]`, `resp[a:b]` and arithmetic/comparison/logic expressions are evaluated by `Engine::MiniExpression`. One parser covers `validCondition`, `derivedLength` and handshake conditions.

```text
expression  = logical_or
logical_or  = logical_and { "||" logical_and }
logical_and = bit_or { "&&" bit_or }
bit_or      = bit_xor { "|" bit_xor }
bit_xor     = bit_and { "^" bit_and }
bit_and     = equality { "&" equality }
equality    = relational { ("==" | "!=") relational }
relational  = shift { ("<" | ">" | "<=" | ">=") shift }
shift       = additive { ("<<" | ">>") additive }
additive    = term { ("+" | "-") term }
term        = unary { ("*" | "/" | "%") unary }
unary       = ("!" | "-" | "~") unary | primary
primary     = number | hex | placeholder | resp_ref | "(" expression ")"
placeholder = "{" name [":" format] "}"
format      = "X" digit+       // fixed-width hex
             | "raw"           // variable-length bytes (P1 A)
             | (default)       // hex/ASCII depending on the name category
resp_ref    = "resp[" [int] [":" [int]] "]"
name        = identifier (letters/digits/underscore, not starting with a digit)
```

| Placeholder form | Entry point | Use case | Example |
|---|---|---|---|
| `{Name}` | `Build` (looks up `tag.variables` / protocol input static values) | simple numeric/byte values | `{RegisterCount}` |
| `{Name:Xn}` | `Build` (n-digit fixed-width hex, zero padded) | 1-8 byte scalars | `{StartAddress:X4}` -> 4-byte big endian |
| `{Name:raw}` | `BuildBytes` (hex string -> `vector<uint8_t>`, inserted verbatim) | variable-length payloads (P1 A) | `{Payload:raw}` |

- `resp[5]` -> one byte as an unsigned integer (0..255).
- `resp[5:9]` -> a 4-byte slice, used by `sessionExtractExpr` to pull a SessionID.
- `resp[]` -> the whole frame; only valid inside `validCondition`, never as a placeholder.
- Precedence matches C: `||` < `&&` < `|` < `^` < `&` < `== !=` < `< > <= >=` < `<< >>` < `+ -` < `* / %` < unary `! - ~`.
- No function calls and no floating point; complex computations use the protocol author's `calc` built-in on the write path.
- Failure handling: unknown placeholder name -> runtime `BuildError` (Bad Quality, request never leaves); `resp[...]` slice out of range -> `ParseError`; invalid hex characters (only the `{Name:raw}` path) -> `BuildError`.

```cpp
struct HandshakeStep {
    std::string name;
    std::vector<std::string> requestTemplate;
    Optional<FramingConfig> framingOverride;        // per-step framing (null/missing = channel default)
    std::string validCondition;                     // success condition
    std::string sessionExtractExpr;                 // e.g. "resp[5:9]"
    std::string sessionVariable;                    // e.g. "SessionID"
    int timeoutMs = 0;                              // 0 = inherit device.connection.timeoutMs
};
```

**Handshake ordering and device lifecycle** (ADR-0010)

State machine (5 states, see [architecture/04 §4.3](../architecture/04_Error_Shutdown_Security.md)): `New -> Connecting -> Connected -> Degraded -> Disabled`.

| Failure point | Transition | Error code | Follow-up |
|---|---|---|---|
| Step 1 | New -> Connecting -> Disabled | `ConnectionRefused` or `Timeout` | device marked Disabled; no retry (needs SIGHUP/reload) |
| Step N (N>1, first N-1 succeeded) | Connecting -> Connecting (retry) | `Timeout` / `InvalidResponse` | total handshake attempts = 1; total timeout = sum(step.timeoutMs) |
| All steps succeed | Connecting -> Connected | (none) | device becomes pollable by TagReader |
| First request after handshake fails | Connected -> Degraded | `Timeout` / `ConnectionClosed` | Degraded, breaker counts |
| Degraded hits `failureThreshold` | Degraded -> Disabled | (none) | marked Disabled, persisted, WebApi alert |

- Any step failing aborts the whole handshake; there is no per-step retry. Operators reconnect through `POST /api/devices/<id>/reconnect`.
- Aborting wholesale avoids dirty state where Step 3 extracted a SessionID but Step 4 never committed it.
- `sessionExtractExpr` failure (slice out of range) -> `InvalidResponse` for that step -> handshake fails.
- `sessionVariable` colliding with `device.username` / `device.password` -> load-time `ConfigError`.
- An empty `validCondition` skips validation (treated as success). Strongly discouraged in production, since "no echo validation" equals "no protocol handshake" (plain Modbus-style TCP).

```cpp
// ────────── protocol configuration ──────────

struct ProtocolConfig {
    std::string protocolName;
    TransportConfig transport;
    FramingConfig framing;
    Optional<ByteOrder> dataByteOrder;                 // tag-level fallback; unset = BigEndian
    std::unordered_map<std::string, OperationConfig> operations;
    std::vector<HandshakeStep> handshake;             // empty = no handshake (Modbus)
    int schemaVersion = kSupportedSchemaVersion;      // generation number (ADR-0005)
};
```

> Template functions and checksums: `crc16modbus` / `crc16ccitt` / `crc32` / `lrc` / `xor8` are built-in capabilities checked against an internal allow-list (name + width). The protocol side declares nothing. Checksums are a protocol-level mechanism — there is no "disable checksum" switch; the placeholder drives computation and a second scan fills it once the length field is known. See [02_Engine](./02_Engine.md) and [ADR-0012](../../adr/0012-derivedlength-template-primitives-and-trial-render.md) *zh*.

```cpp
// ────────── device configuration ──────────

struct ConnectionConfig {
    std::string host;              // Tcp/Tls: host name or IP
    uint16_t port = 0;             // Tcp/Tls: 0 = protocol defaultPort
    std::string portName;          // Serial: overrides the protocol-level portName
    int timeoutMs = 3000;          // connect-establishment timeout
};
```

| Field | Phase | Scope | Failure mapping |
|---|---|---|---|
| `connection.timeoutMs` | **establish** the TCP/TLS/serial handle | one connect() call | -> `ConnectionRefused` (retryable) |
| `device.requestTimeoutMs` | **one request/response round trip** | one SendReceive() call | -> `Timeout` (retryable) or `WriteTimeout` (writes: no retry) |

> The two are strictly separated. Handshake uses `connection.timeoutMs` (or a step-level `timeoutMs > 0`); business reads/writes use `device.requestTimeoutMs`. The loader does **not** compare their magnitudes.

```cpp
/// Resilience policy (Config_Schema §11 / ADR-0004). Device-level overrides fall back per field.
struct ResilienceConfig {
    int maxAttempts = 3;           // total read attempts (incl. first); writes are always 1
    int backoffBaseMs = 100;       // exponential backoff base
    int backoffMaxMs = 1000;       // per-attempt backoff cap
    int failureThreshold = 5;      // consecutive logical read failures -> breaker opens
    int cooldownMs = 10000;        // breaker open duration; CircuitOpen fast-fails meanwhile
    int halfOpenProbes = 1;        // probes in half-open state
};
```

| Field | Global default | Device unset | Device = 0 | Device > 0 |
|---|---|---|---|---|
| `maxAttempts` | 3 | global | global | device |
| `backoffBaseMs` | 100 | global | global | device |
| `backoffMaxMs` | 1000 | global | global | device |
| `failureThreshold` | 5 | global | global | device |
| `cooldownMs` | 10000 | global | global | device |
| `halfOpenProbes` | 1 | global | global | device |

> Fields are plain `int` rather than `Optional<int>` so JSON parsing stays trivial ("missing" and "0" both mean "not set"). The one exception: writes are forced to a single attempt regardless of configuration; a device-level `maxAttempts > 1` combined with a write-named operation produces a **Warning**.

```cpp
/// Management-plane security (Config_Schema §12 / ADR-0008). Optional top-level block; nullopt = all defaults.
struct WebApiConfig {
    std::string bindAddress = "127.0.0.1";  // default loopback only
    std::string certFile;                   // reserved for TLS; plain HTTP in v1
    std::string keyFile;                    // paired validation only
    bool requireAuth = false;               // true = fail-fast when the token is missing
    int rateLimitRps = 5;                   // token bucket refill rate
    int rateLimitBurst = 10;                // burst capacity; exceeding it -> 429
    // token comes from MYPROT_API_TOKEN, never from the file; masked in logs
};
```

| Field | Default | Range / behaviour | Load-time validation |
|---|---|---|---|
| `bindAddress` | `"127.0.0.1"` | IP literal; `"0.0.0.0"` = all interfaces (needs `requireAuth=true`) | non-empty; invalid IP -> `ConfigError` |
| `certFile` | `""` | PEM path | must be set together with `keyFile` |
| `keyFile` | `""` | PEM path | same |
| `requireAuth` | false | true = enforce `Authorization: Bearer $MYPROT_API_TOKEN` | missing env var -> startup fail-fast |
| `rateLimitRps` | 5 | 0.1 - 1000; 0 = disabled (not recommended) | < 1 or > 1000 -> `ConfigError` |
| `rateLimitBurst` | 10 | 1 - 100 | outside range -> `ConfigError` |

> Rate limiting: steady 5 RPS, burst 10, then back to steady state. Over-limit returns HTTP 429 with `Retry-After: 1`. Limiting applies per source IP (socket peer; `X-Forwarded-For` is not trusted).

```cpp
struct DeviceConfig {
    std::string id;
    std::string protocol;
    ConnectionConfig connection;           // timeoutMs covers connect establishment only
    int requestTimeoutMs = 3000;           // one request/response round trip, shared by all operations
    Optional<std::string> username;        // protocol credentials; injected as template vars, masked in logs
    Optional<std::string> password;
    Optional<ResilienceConfig> resilience; // per-device override; unset = ConfigRoot.resilience
};

// ────────── tag definition ──────────

struct TagDefinition {
    std::string name;                   // globally unique (e.g. "PLC-001.Temperature")
    std::string label;                  // display name; metadata only
    std::string unit;                   // engineering unit; metadata only
    std::string deviceId;
    std::string operation;              // e.g. "ReadHoldingRegisters"
    // Cross-protocol byte units (v1.25/v1.28): StartByteAddress / ByteCount;
    // protocol-family units (StartAddress/RegisterCount) are derived by protocol outputs.
    std::unordered_map<std::string, uint32_t> variables;
    int scanRateMs = 1000;
    std::string finalType = "UInt16";
    Optional<ByteOrder> byteOrder;      // unset -> protocol dataByteOrder -> BigEndian
    bool coalesce = true;               // participate in address-adjacency merging
    int bitOffset = -1;                 // 0-7 within the byte (-1 = undeclared)
    std::string direction = "read";     // "read" | "write" (write-only tags never poll)
    std::string writeOperation;         // scalar write (POST value); empty = not writable
    std::string writeBytesOperation;    // variable-length write (POST bytes); empty = not writable
    // deadband / reportMode / variableBytesHex have been removed
};

// ────────── configuration root (tags.json) ──────────

struct ConfigRoot {
    int schemaVersion = 1;                       // generation number (ADR-0005)
    Optional<ResilienceConfig> resilience;       // global policy; unset = all defaults
    Optional<WebApiConfig> webApi;               // management-plane security; unset = all defaults
    std::vector<DeviceConfig> devices;
    std::vector<TagDefinition> tags;
};
```

> Note: the implementation additionally carries `TagDefinition::converters` (`std::vector<Core::ScaleConverter>`, v5 C1, `value' = value*k + b`, result always `Double`). `ProtocolJsonParser` validates the shape and `SemanticChecker` rule 13 restricts converters to numeric `finalType`s and forbids them on write tags.

### 1.2.1 Write operation name convention (ADR-0007)

Writes add no schema fields; they are identified by the literal `OperationConfig.name`. Protocol authors declare write operations under the agreed names and `TagReader::WriteOnce` / `WriteBytes` look them up by name.

| Operation name | Use | Auto-injected variables | responseParser usage | Note |
|---|---|---|---|---|
| `WriteSingleRegister` | single register write (FC06) | `StartAddress` (tag), `WriteValue` (scalar uint32) | echo validation only (`validCondition`, e.g. `resp[7] == 0x06`), no payload parsing | main scalar-write entry |
| `WriteMultipleRegisters` | multi register write (FC16) | `StartAddress`, `RegisterCount`, `RegByteCount` (derived from payload length) | same (`resp[7] == 0x10`) | main variable-length write entry (`{Name:raw}`) |

**Protocol author contract**

- `requestTemplate` uses the same placeholder syntax as reads: `{Name:Xn}` and `{Name:raw}`.
- `responseParser.validCondition` must do strict echo validation; the payload area is not parsed.
- A write operation does not need to be referenced by any `TagDefinition.operation`. Unreferenced write operations produce no "unused" warning.
- Rule 14 (Config_Schema §7) validates only operations referenced by tags; write-time injected variables (`WriteValue`, `RegisterCount`) never trigger "template variable undefined" at load time.

### 1.2.2 schemaVersion loader gate (ADR-0005)

`schemaVersion` on `ConfigRoot` and on every `ProtocolConfig` inserts a **version gate** into `JsonConfigLoader`, which must run before field-level validation.

```text
LoadConfig(jsonPath)
  |
  [1] parse JSON -> in-memory object (nlohmann::json)
  |
  [2] read root schemaVersion
  |
  [3] version gate:
      if missing        -> log("schemaVersion missing, assuming 1") -> continue
      elif == kSupportedSchemaVersion (=2) -> continue
      elif <  kSupportedSchemaVersion -> ConfigError("configuration too old ...")
      elif >  kSupportedSchemaVersion -> ConfigError("configuration from a newer version ...")
  |
  [4] repeat [2][3] for every protocols/*.json; mismatch with the root -> ConfigError
  |
  [5] field-level ConfigValidator (types / required fields / usage matrix)
  |
  [6] produce strongly typed POCOs (ConfigRoot + ProtocolConfig/DeviceConfig/TagDefinition)
```

> The gate runs before field validation so an old configuration does not produce a pile of meaningless field errors (ADR-0005 §3).

### 1.2.4 Tag/operation reference matrix (startup validation, cycle detection)

| Relation | Source | Target | Load-time validation | Runtime lookup |
|---|---|---|---|---|
| tag -> op | `TagDefinition.operation` | `operations[name]` | must exist; the op must be a read operation (name not starting with `Write`) | by `op.name` at dispatch |
| tag -> device | `TagDefinition.deviceId` | `devices[id]` | must exist; protocol consistency (`device.protocol` == the op's protocol) | by `deviceId` at dispatch |
| handshake -> op | `HandshakeStep.requestTemplate` | `operations[name]` (runtime template) | **not** strictly validated (handshake is protocol-scoped) | per step |
| write op -> tag | `tag.operation` | — | **none** (writes are never referenced by tags) | direct table lookup by WebApi |
| expression `{Name}` | placeholder `name` | `tag.variables` / protocol input statics / handshake session vars | load time validates **read** operations only | at render time |
| `sessionVariable` | handshake step | later `requestTemplate` placeholders | must be **forward** reachable (Step N references Step M<N) | at render time |

**Cycle detection**

- `TagDefinition` -> `OperationConfig` is unidirectional; cycles cannot occur.
- Placeholders inside `requestTemplate` reference variables only, never other operations.
- `HandshakeStep.sessionVariable` must be forward reachable; a backward reference (Step 1 using a Step 3 variable) is a `ConfigError`.
- Protocol authors do not manage inter-step references: the loader keeps an in-memory `session_var_pool` accumulated in step order.

**Orphan operations**

- Orphan read operation -> load-time **Warning**.
- Orphan write operation -> **no** warning (writes are addressed by name from WebApi).
- Orphan handshake steps are reported only when `operations` outnumbers the ops referenced by handshakes.

**Protocol vs global names**: operation names are unique within a protocol (`unordered_map` key) and may repeat across protocols (`TagDefinition.protocol` namespaces them).

### JSON configuration example

> Full schema (all fields, defaults, template grammar, validation rules) lives in [Config_Schema.md](../Config_Schema.md). Only a minimal runnable fragment appears here; JSON field names map one-to-one onto the POCOs above.

**tags.json** (excerpt — historical; `registerCount`, `deadband` and `reportMode` have since been removed):

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
      "finalType": "Float"
    }
  ]
}
```

**configs/protocols/modbus-tcp.json** (excerpt):

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

> Placeholders: `{Length:calc:X4}` is filled during the engine's second scan with the byte count after that field (6 here); `{TransactionID:auto:X4}` is an auto-increment transaction id as 2 bytes big endian; `"03"` is a hex literal (space-separated byte sequence) carrying the function code; the response payload is `resp[9 .. 9+resp[8]]`, i.e. after MBAP (7) + function code (1) + byte count (1).

## 1.3 Value type system

```cpp
// src/Core/include/MyProt/Core/Value.hpp

enum class QualityCode { Good, Bad, Uncertain };

enum class ValueType {
    Empty,      // only used with Bad/Uncertain quality
    ByteArray,
    UInt16, Int16, UInt32, Int32, UInt64, Int64,
    Float, Double,
    Bool,
    String
};

/// Typed value — tagged union (replaces std::variant, ADR-0010 §3).
/// `type` selects the storage field; integers are stored widened
/// (UInt16->u, Int16->i) and float is widened to double (lossless).
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
    std::vector<uint8_t> rawData;       // kept on Uncertain for diagnostics (ADR-0006)
    QualityCode quality = QualityCode::Good;
    Error lastError;
    int64_t timestamp;                // epoch ms
    int64_t requestId;                // end-to-end correlation id (ADR-0009)
    bool valueChanged = false;        // computed by LatestValueStore::Update (producers never set it)
};
```

### 1.3.1 QualityCode semantics and Error -> Quality mapping (ADR-0006)

Three values, split by "can we talk to the device" and "is the data trustworthy" (full ruling in [ADR-0006](../../adr/0006-quality-semantics.md) *zh*):

| Quality | Meaning | typedValue | rawData |
|---|---|---|---|
| `Good` | full success (`validCondition` passed, conversion succeeded) | valid value | copy of the raw bytes |
| `Uncertain` | device online and responding, but this value cannot be fully trusted | usually `Empty` | **raw bytes preserved** |
| `Bad` | no usable data (communication failure / request never sent / unparsable frame / config or internal error) | `Empty` | empty |

> Ruling: can talk but answered nonsense -> `Uncertain`; cannot talk at all or the request never left -> `Bad`; everything fine -> `Good`.

`InvalidResponse` and `TypeConversionError` map to `Uncertain`; every other code maps to `Bad`; the success path assigns `Good`. Transport errors produce no `TagValue` inside the retry window and emit a `Bad` value once the budget is exhausted (see [ADR-0004](../../adr/0004-timeout-retry-budget.md) *zh*).

#### 1.3.1.1 Boundary-case rulings

| Case | Error::Code | Quality | Rationale |
|---|---|---|---|
| Bytes received but `validCondition` failed | `InvalidResponse` | `Uncertain` | online, responding, but answered the wrong question |
| Bytes received but `finalType` conversion failed | `TypeConversionError` | `Uncertain` | online, responding, valid bytes but unusable semantics |
| `validCondition` passed but frame exceeds `maxFrameSize` | `InvalidResponse` | `Uncertain` | treated as a dirty response |
| Out-of-range while parsing (`dataStartIndex`) | `ParseError` | `Bad` | protocol shape error |
| `BuildError` (template render failed) | `BuildError` | `Bad` | the request never left |
| Write echo timeout | `WriteTimeout` | `Bad` | writes are non-idempotent -> no retry (strictly separate from read `Timeout`) |
| Write echo OK but read-back mismatch (P1 B) | `ReadBackMismatch` | `Bad` | written but wrong; 503 alert |
| Missing config / protocol (startup) | `ConfigError` / `ProtocolNotFound` | (no TagValue) | startup fail-fast |
| Circuit breaker open | `CircuitOpen` | `Bad` | fast failure, last callback after budget exhaustion |
| Repeated `Timeout` inside the retry window | (after exhaustion) `Timeout` | `Bad` | no TagValue inside the window |
| Write `validCondition` passed (echo OK) | (none) | `Good` | success path assigns Good directly, no `typedValue` |

> Important: the quality ruling looks at `Error::Code` only. Modules must not hand-write `if (e.code == X) quality = Good` outside the success path.

**`{Name:raw}` rendering (variable-length bytes)**

A `{Payload:raw}` placeholder consumes runtime-injected payload bytes: the write path puts `POST /api/data/write`'s `bytes` (or a `value` encoded per `finalType`) into the raw table under `writeVariable` (default `WriteValue`), and `RequestBuilder` parses the hex into `std::vector<uint8_t>` during `BuildBytes`. `raw` is the placeholder's second segment, **not** a schema field; configuration only has the scalar `variables` map (the old `variableBytesHex` field was removed in v1.32 — its declared value never reached the frame). Fixed-width placeholders route through `Build`, variable-width ones through `BuildBytes`.

**P1 B read-back loop (2026-08-28)**

`TagReader::WriteOnce` / `WriteBytes` take a `bool readBack` parameter (default false; WebApi enables it via `"readBack": true`). After echo validation succeeds, an immediate read of the same address is issued; a mismatch yields `Error::Code::ReadBackMismatch` (`IsRetryable` = false to avoid a second write; mapped to `Bad`; HTTP 503). Protocols without a `ReadHoldingRegisters` operation skip read-back silently (treated as unsupported, **not** degraded to 503). See [05_Gateway §5.3](./05_Gateway.md), [07_WebApi §7.5](./07_WebApi.md), [ADR-0007](../../adr/0007-write-path-scope.md) *zh*.

### 1.3.3 Write tri-state errors are never retried

| Error::Code | Trigger | Consequence of retry | Policy |
|---|---|---|---|
| `WriteTimeout` | write budget exhausted (no echo) | device may or may not have applied it | **no retry** |
| `WriteFailed` | echo validation failed | known not applied | **no retry**, manual investigation |
| `ReadBackMismatch` | echo OK, read-back differs | applied but wrong value | **no retry**; 503 alert |

`IsRetryable` returns false for all three, so the read retry loop can never trigger a write. WebApi's `POST /api/data/write` always uses `attempt == 1`.

### 1.3.4 TagValue field flow

`TagValue` is the core payload of the data flow: `TagReader` produces it, `PollingEngine::ResultDispatch` hands it straight to consumers (LatestValueStore + WebApi `/api/data/latest`; the standalone `DataDispatcher` and deadband layer were withdrawn in v1, see [06_Polling §6.2](./06_Polling.md)).

| Field | Filled when | Good path | Uncertain path | Bad path |
|---|---|---|---|---|
| `tagName` | on entry | yes | yes | yes (Bad values are still emitted for diagnostics) |
| `deviceId` | on entry | yes | yes | yes |
| `typedValue` | after parse/convert | valid value per `finalType` | usually `Empty` | `Empty` |
| `rawData` | right after parsing | raw copy | **preserved** (core contract, distinguishes it from Bad) | empty |
| `quality` | on exit | `Good` | `Uncertain` | `Bad` |
| `lastError` | on exit | `Error{Good, "", ""}` sentinel | `InvalidResponse` / `TypeConversionError` | `Timeout` / `ConnectionClosed` / `CircuitOpen` etc. |
| `timestamp` | parse success / before failure callback | local arrival time | same | failure callback time |
| `requestId` | assigned at TagReader entry (atomic counter) | shared by merged requests | same | same |
| `valueChanged` | **not filled here**: producers (`TagReader` / `TagValueSink`) never set it; `LatestValueStore::Update` computes it at write time (see §1.3.6) | decided when written to the store | same | same |

> Consumer contract: only `Good` tags may be used as business values. `Uncertain` tags should keep `rawData` for "reference the old value + show a degraded flag" strategies and do not count toward the Good ratio. `Bad` tags may drop `typedValue` but **must** be retained to raise alerts. `requestId` must survive into persistence so a logical read can be traced end to end.

### 1.3.5 TypedValue <-> finalType mapping

`tag.finalType` is a config string; [ResponseParser::Parse](./02_Engine.md) parses it and calls `Core::FromBytesU16/U32/U64` directly to fill `TypedValue.type` (the standalone `ConvertToFinalType` helper was withdrawn — item A2).

| `finalType` string | `TypedValue.type` | Read field | Write field | Width (bytes) |
|---|---|---|---|---|
| `"Bool"` | `ValueType::Bool` | `b` | `b` | 1 |
| `"UInt16"` | `ValueType::UInt16` | `u` | `u` (truncated to 16 bits) | 2 |
| `"Int16"` | `ValueType::Int16` | `i` (16-bit bit-reinterpretation) | `i` (truncated) | 2 |
| `"UInt32"` | `ValueType::UInt32` | `u` (low 32 bits) | `u` (truncated to 32) | 4 |
| `"Int32"` | `ValueType::Int32` | `i` (32-bit bit-reinterpretation) | `i` (truncated) | 4 |
| `"UInt64"` | `ValueType::UInt64` | `u` | `u` | 8 |
| `"Int64"` | `ValueType::Int64` | `i` | `i` | 8 |
| `"Float"` | `ValueType::Float` | `d` (lossless widening) | `d` (narrowed on write) | 4 |
| `"Double"` | `ValueType::Double` | `d` | `d` | 8 |
| `"ByteArray"` | `ValueType::ByteArray` | `bytes` | `bytes` | variable |
| `"String"` | `ValueType::String` | `str` | `str` | UTF-8 byte count |
| anything else / missing | `ValueType::Empty` | (none) | (none) | 0 |

> Signed types share the storage field width of their unsigned counterparts and switch via `static_cast`. The write path casts through `uint32_t` to avoid C++11 implementation-defined negative conversions (see the end of §1.4).
> `Bool`: `bytes[0] == 0x00` -> false, non-zero -> true, zero bytes -> `TypeConversionError` (`Uncertain`).
> `String`: decoded as UTF-8; failure -> `TypeConversionError` (`Uncertain`), never truncated at the first invalid byte.

### 1.3.6 Unified valueChanged computation (LatestValueStore)

`valueChanged` has exactly **one place**: [`LatestValueStore::Update`](./06_Polling.md#change-detection) computes it against the previously written sample. Producers (polling `TagReader`, southbound `TagValueSink`) never fill it, which removes the old split semantics ("always false on the polling path, self-computed on the push path").

```
changed = first write
       || quality != previous quality
       || !TypedValueEquals(typedValue, previous typedValue)
```

| Scenario | valueChanged | Notes |
|---|:--:|---|
| First write for that tag | `true` | no baseline yet |
| Only `timestamp` / `requestId` differ, value and quality equal | `false` | neither field is compared (a fast poll never looks "changed") |
| Different type or payload | `true` | per-type rules below |
| Quality transition (incl. `Good->Uncertain`, `Good->Bad`) | `true` | identical value still counts (upstream must see the degradation) |
| Consecutive `Uncertain` / consecutive `Bad` with the same payload | `false` | neither quality nor payload moved |
| First write after `Clear()` | `true` | baseline dropped, back to "first write" |

`TypedValueEquals` (`Core/Value.hpp`) dispatches by type:

| `ValueType` | Comparison | Notes |
|---|---|---|
| `Empty` | always equal | two empty values are not a change |
| `ByteArray` | byte-wise | a different length or any different byte counts |
| `UInt16/32/64`, `Int16/32/64` | integer equality | — |
| `Float/Double` | floating-point equality (`==`) | **NaN != NaN** -> every NaN sample counts as changed (usable as an anomaly alarm); `-0.0 == 0.0` is not a change |
| `Bool` | boolean equality | — |
| `String` | exact string equality | case sensitive |
| different types | never equal | e.g. `UInt16(1)` vs `Int16(1)` |

> **Why not in the producer**: "changed" is relative to the last sample actually stored, and only the store holds that baseline; polling and push also share one store, so the definition must be single.
>
> **The former `deadband` / `reportMode` are gone** (no consumer, see [ADR-0003](../../adr/0003-known-issues.md)): v1 does no deadband suppression - any value/quality change sets the flag. Consumers that only want deltas filter on `changed` themselves (MQTT `publishMode=changed` does exactly that, see [modules/11](./11_Northbound.md)).

### 1.3.7 timestamp source and trust in peer clocks

`TagValue.timestamp` is always **local epoch milliseconds** (`std::chrono::system_clock::now()`); peer device time is never used (device clock drift is untrustworthy).

| Phase | Source | Purpose |
|---|---|---|
| Good | local arrival time of the response | upstream timelines, Timescale persistence |
| Uncertain | end of parsing (never the peer timestamp field) | same |
| Bad | failure callback time | upstream alert timeline |

> The protocol layer does no NTP synchronisation, and Modbus / S7 / MELSEC carry heterogeneous timestamp fields (CP37/CP40/DBW0 ...), so nothing is abstracted. A separate `TagValue.remoteTimestamp` is tracked in [ROADMAP](../../ROADMAP.md) *zh*.
> Across hosts, `timestamp` is an event sequence marker only — never the basis for merging two hosts' data. Aggregation correlates by `requestId` on the Timescale side.

## 1.4 ByteOrder

Four orders plus numeric <-> wire-byte conversion. The implementation is **independent of host endianness** (pure shift assembly, no `reinterpret_cast`, no reliance on memory layout).

```cpp
// src/Core/include/MyProt/Core/ByteOrder.hpp

enum class ByteOrder {
    BigEndian,          // standard big endian
    LittleEndian,       // standard little endian
    WordBigByteLittle,  // mixed CDAB: low word first, big endian within the word (Melsec)
    WordLittleByteBig   // mixed BADC: high word first, little endian within the word
};

// Write path: value -> wire bytes
inline std::vector<uint8_t> ToBytes(uint16_t val, ByteOrder order);
inline std::vector<uint8_t> ToBytes(uint32_t val, ByteOrder order);
inline std::vector<uint8_t> ToBytes(uint64_t val, ByteOrder order);

// Read path: wire bytes -> value (short input returns 0)
inline uint16_t FromBytesU16(ByteView data, ByteOrder order);
inline uint32_t FromBytesU32(ByteView data, ByteOrder order);
inline uint64_t FromBytesU64(ByteView data, ByteOrder order);
```

**Wire semantics** (u32 = 0x12345678; `wa` = first two bytes read big-endian, `wb` = last two bytes read big-endian):

| ByteOrder | Wire bytes | Assembly (u32) |
|---|---|---|
| `BigEndian` | `12 34 56 78` | `(wa<<16) \| wb` |
| `LittleEndian` | `78 56 34 12` | plain little-endian read |
| `WordBigByteLittle` (CDAB) | `56 78 12 34` | low word first, big endian within word -> `(wb<<16) \| wa` |
| `WordLittleByteBig` (BADC) | `34 12 78 56` | high word first, little endian within word -> `(w1<<16) \| w0` |

> A single 16-bit word has no word order: apart from `LittleEndian`, everything is emitted and parsed big-endian. Mixed orders are mutual inverses, so a write/read round trip is exact; u64 mixed orders apply the same rule to two 32-bit word groups.

**u64 mixed-order expansion** (`wa` = bytes[0..3] big-endian within the word, `wb` = bytes[4..7]):

| ByteOrder | u32 wire bytes | u64 assembly |
|---|---|---|
| `BigEndian` | `12 34 56 78` | `(u32hi<<32) \| u32lo`, `u32hi = 0x12345678` |
| `LittleEndian` | `78 56 34 12` | plain little-endian read |
| `WordBigByteLittle` (CDAB) | `56 78 12 34` | `(u32lo<<32) \| u32hi` |
| `WordLittleByteBig` (BADC) | `34 12 78 56` | `(reverse(u32lo)<<32) \| reverse(u32hi)` |

> Mixed orders exist so one wire sequence can be understood by both big- and little-endian protocols — typically Melsec floats (`WordBigByteLittle`) and some PLCs (`WordLittleByteBig`).

**Signed values**: `ToBytes` accepts unsigned types; callers widen then bit-reinterpret, e.g. `static_cast<uint32_t>(static_cast<int32_t>(v))`. This avoids implementation-defined negative conversions in C++11. Reads return unsigned values that the caller casts back with `static_cast<int32_t>` (bit-reinterpretation, equivalent to C++20 `std::bit_cast`).

#### 1.4.1 ByteView boundary behaviour

`ByteView` replaces `std::span<const uint8_t>` (avoiding a C++20 dependency) and is adapted from `asio::mutable_buffer`. Short input returns 0 and never throws.

| Call | Input length | Return | Behaviour |
|---|---|---|---|
| `FromBytesU16(view, BigEndian)` | >= 2 | parsed | normal |
| `FromBytesU16(view, BigEndian)` | 0 or 1 | `0` | **no error** |
| `FromBytesU32(view, ...)` | < 4 | `0` | same |
| `FromBytesU64(view, ...)` | < 8 | `0` | same |
| `FromBytesU16(view, ...)` | empty (null) | `0` | no crash (assert in debug) |

> Rationale: TagReader validates `dataLengthExpr` ranges on entry, so FromBytes is only reached for the trailing bytes of a frame. Returning 0 lets upstream recognise the common "zero-padded tail" shape (e.g. the last register of a multi-register Modbus read).

#### 1.4.2 ToBytes truncation

`ToBytes` explicitly truncates to N bytes when the value is wider than the target type, mirroring FromBytes.

| Call | Input | Wire bytes | Behaviour |
|---|---|---|---|
| `ToBytes(uint16_t(0x1234), BigEndian)` | 0x1234 | `12 34` | normal |
| `ToBytes(uint32_t(0xCAFEBABE), BigEndian)` | 0xCAFEBABE | `CA FE BA BE` | 4 bytes |
| `ToBytes(uint64_t(...), BigEndian)` | any 64-bit | 8 bytes | — |
| `ToBytes(uint16_t(0x12345678), BigEndian)` | wider than 16 bits | `56 78` | **truncated**, high 16 bits lost |

> Callers are contracted to size the input to N bytes; `v1` performs no implicit narrowing (a `uint64` -> `uint16` conversion requires an explicit `static_cast`, which lets the compiler emit a truncation warning).

#### 1.4.3 Host-endianness independence self-check

`ToBytes` / `FromBytes` use shift assembly and lookup tables only — no `htonl` / `ntohl` / cross-type `memcpy`.

```cpp
// Example: FromBytesU32 (BigEndian)
uint32_t FromBytesU32(ByteView v, ByteOrder o) {
    if (o == ByteOrder::BigEndian) {
        return (uint32_t(v[0]) << 24) | (uint32_t(v[1]) << 16)
             | (uint32_t(v[2]) <<  8) |  uint32_t(v[3]);
    }
    // ... other orders analogous
}
```

> No `htonl` fast path on big-endian hosts: cross-platform behaviour must be 1:1 identical, and the runtime branch is folded away by the compiler.

---

> **Next**: [Engine module](./02_Engine.md)

---

# Part II: Cross-cutting concerns

> Eight independent contract layers, all under `src/Core/include/MyProt/Core/`. **Every module** that includes Core must obey this part. Part I described **data**; this part describes **mechanisms**.

## 1.5 Optional\<T\> — optional-value container (hand-written)

> Why not `std::optional`:
> - VS2015 v140 defaults to C++14; enabling C++17 needs `/std:c++17`, which the v140 toolset does not support (only v141/v142).
> - Third-party polyfills add dependencies, violating ADR-0010's zero-dependency rule.
> - A 140-line hand-written version covers 30 call sites in 11 files: dependency-free, controllable, readable.
>
> Constraint: T must be default-constructible; a `const` member in T is a compile error.

```cpp
// src/Core/include/MyProt/Core/Optional.hpp
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
    detail::OptionalStorage<T> _storage;  // union storage, zero overhead
};
```

> API mirrors `std::optional`: `has_value()` / `value()` / `value_or(fb)` / `reset()` / `emplace(args...)` / `operator bool` / `operator->` / `operator*` / `operator==` / `operator!=` / `nullopt` / `nullopt_t`. The only difference: `value()` asserts in debug and does not throw in release (project convention).

### 1.5.1 Optional API contract

| Interface | Behaviour | Failure mode | Difference from `std::optional` |
|---|---|---|---|
| default ctor | `_hasValue = false` | T not default-constructible -> compile error | same |
| copy ctor | deep copy | T not copyable -> compile error | same |
| move ctor | noexcept if T's move is | depends on T | same |
| `has_value()` / `operator bool()` | explicit bool | — | same |
| `value()` | assert | debug trap; release returns uninitialised storage | throws `bad_optional_access` |
| `value_or(fallback)` | value or fallback | — | same |
| `operator->` / `operator*` | assert | as `value()` | `std::optional` does not assert |
| `reset()` | clears + `T()` | — | same |
| `emplace(args...)` | in-place construction | as above | same |

### 1.5.2 Fail-Open vs Fail-Close

Two usages in Config POCOs, with two different default behaviours:

**Fail-Close (nullopt means refuse)**
- Applies to `DeviceConfig.username` / `password` / `resilience`, `Optional<FramingConfig>`.
- nullopt = "not configured" = "use global default / skip / disable".
- Callers must check `if (cfg.username)` before dereferencing; otherwise they take the fallback path.

**Fail-Open (nullopt means allowed)**
- Applies to `ResilienceConfig` fields and device-level `connection` overrides.
- nullopt = "not set" = "fall back to protocol/global level".
- The loader merges "global block + device override" (see the ResilienceConfig table in §1.2).

### 1.5.3 Semantic boundary with Expected\<T\>

| Container | Expresses | Use for |
|---|---|---|
| `Optional<T>` | "the **value may not exist**" (e.g. an optional config item) | configuration / defaults |
| `Expected<T>` | "the **operation may fail**" (e.g. device unreachable) | protocol / I/O / validation |

- `Optional<Error>` is forbidden (neither "config default" nor "operation failure").
- `Expected<Optional<T>>` is allowed (rare: the operation succeeded but this sample has no value).

### 1.5.4 Cost

| Shape | Size | Copy cost |
|---|---|---|
| `Optional<uint16_t>` | 4 bytes (bool + padding + uint16_t) | 4 bytes |
| `Optional<std::string>` | 40+ bytes | string copy |
| `Optional<std::vector<uint8_t>>` | 32 bytes | vector copy, O(N) |

> Large types are still stored by value (matching `std::optional`). Hot copy paths (e.g. per-frame TagValues) should switch to `Optional<std::shared_ptr<T>>`.

### 1.5.5 Not-usable scenarios

1. `T` is a reference type -> must be default-constructible.
2. `T = void` -> use a `bool` indicator instead.
3. Polymorphic base plus value slicing -> use `std::unique_ptr<Base>`.
4. `T` has a `const` member -> not default-constructible.
5. C++17 `if (auto x = opt)` does not apply (explicit bool) -> use `if (opt.has_value())`.
6. Sharing `Optional<T>` across threads -> unsafe, same as `Expected<T>`.

---

## 1.6 ByteView — immutable byte view (hand-written, C++11)

> Hand-written instead of `std::span<const uint8_t>` (C++20) / `gsl::span` (dependency problems). Zero copy; holds only `const uint8_t*` + size; **never owns** the memory.

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

### 1.6.1 API contract

| Interface | Behaviour | Failure mode | Difference from `std::span` |
|---|---|---|---|
| default ctor | `_data = nullptr, _size = 0` | — | same |
| from `vector<uint8_t>` | borrows the vector's pointer (**view dies with the vector**) | — | same |
| `data()` / `size()` | read-only access | — | same |
| `operator[]` | assert | debug trap; release undefined | `std::span` does not assert |
| `at(i)` | **throws `std::out_of_range`** | throws | same |
| `subview(off, len)` | sub-view; `len = npos` means "to the end" | asserts offset bounds | same |
| `begin()` / `end()` | random-access iterators | — | same |

### 1.6.2 Lifetime contract (**critical**)

```text
lifetime(ByteView) is a subset of lifetime(underlying buffer)
```

| Holding pattern | Risk | Mitigation |
|---|---|---|
| pointer into `std::vector<uint8_t>` | invalidated by reallocate / resize | hold only while the vector is untouched, or copy |
| pointer into `asio::mutable_buffer` | invalid once the buffer leaves asio's scope | hold only inside the asio handler |
| pointer to a stack array | invalid after the frame returns | never return across functions; copy into a vector |
| passing across threads | owner lifetime not aligned | document it or switch to `shared_ptr<vector<uint8_t>>` |

### 1.6.3 Not-usable scenarios

1. Holding across an async callback -> use `shared_ptr<vector<uint8_t>>`.
2. Mutating the bytes -> ByteView is `const`; use `vector<uint8_t>`.
3. Viewing a temporary -> its lifetime ends with the expression.
4. Capturing by value in a C++11 lambda -> the underlying vector may already be gone.
5. Serialisation -> needs ownership; use `vector<uint8_t>`.
6. As a hash key -> address instability (no `std::hash<ByteView>`).

### 1.6.4 Conversions

| Direction | Path | Copy |
|---|---|---|
| `vector` -> `ByteView` | implicit construction | zero copy |
| `ByteView` -> `vector` | `vector<uint8_t>(view.begin(), view.end())` (`to_vector()` is not implemented) | O(N) |
| `std::array<uint8_t, N>` -> `ByteView` | `ByteView(arr.data(), N)` | zero copy |
| `as_bytes(struct)` -> `ByteView` | manual reinterpret — **forbidden** (see 1.6.5) | — |

### 1.6.5 strict-aliasing risk

```cpp
// ❌ wrong
struct Foo { uint32_t a; };
Foo f;
ByteView v(reinterpret_cast<const uint8_t*>(&f), sizeof(f));  // strict-aliasing UB!
```

Alternatives: convert field by field through `ToBytes` / `FromBytes`; if `memcpy` is genuinely needed, `memcpy(buf, &f, sizeof(f))` first and then build the view over `buf`.

---

## 1.7 Logging (hand-written ConsoleLogger)

> v1 introduces no third-party logger (A8 spdlog withdrawn: zero vendored references, long vcpkg chain). The replacement is a lightweight ConsoleLogger in `src/Core/include/MyProt/Core/Log.hpp` + `src/Core/src/Log.cpp`: 5 levels (debug/info/warn/error/critical), thread-id tag, and simple brace formatting (no nesting; `{}` count must match the argument count).
> Four global macros replace spdlog's six:

```cpp
#define MYPROT_LOG_DEBUG(...)   MyProt::Core::Log::Debug(__VA_ARGS__)
#define MYPROT_LOG_INFO(...)    MyProt::Core::Log::Info(__VA_ARGS__)
#define MYPROT_LOG_WARN(...)    MyProt::Core::Log::Warn(__VA_ARGS__)
#define MYPROT_LOG_ERROR(...)   MyProt::Core::Log::Error(__VA_ARGS__)
```

### 1.7.1 Level boundaries

| Level | Purpose | Frequency | Production default |
|---|---|---|---|
| `DEBUG` | detailed debugging data (parsed bytes, state transitions) | high | **off** (default INFO) |
| `INFO` | startup/shutdown/device state transitions/config reload | low | **on** |
| `WARN` | recoverable anomalies (retrying, first read failure) | low | **on** |
| `ERROR` | business failures (write failure, ReadBackMismatch) | low | **on** |
| `CRITICAL` | impossible internal errors (`InternalError` / `NotImplemented`) | very low | **on** |

> No `TRACE`: it was merged into DEBUG when the spdlog macros were withdrawn. Hot-path cost is handled by compilation — `ConsoleLogger::Debug` returns immediately under `NDEBUG`.

### 1.7.2 Field masking (manual)

The spdlog formatter's automatic masking was withdrawn. `ConsoleLogger` does not inspect field names; callers pass `mask = true` when building the error:
- Field set: `password` / `passwd` / `secret` / `token` / `apiKey` / `sessionId` / `authToken`.
- `Error::Make(code, message, ctx, /*mask*/true)` replaces those values with `***` inside `Error::ToString`.
- A raw string literal such as `"password=xxx"` is **not** masked automatically; do not concatenate secrets into log lines.

### 1.7.3 Explicit correlationId

The spdlog `thread_local` auto-injection was withdrawn. Loggers take the id explicitly:

```cpp
void TagReader::OnResponse(int64_t reqId, ...) {
    MYPROT_LOG_INFO("[reqId={}] tag={} value={}", reqId, tagName, value);
}
```

Cross-thread propagation happens through `TagValue.requestId` ([ADR-0009](../../adr/0009-correlation-tracing.md) *zh*); downstream consumers persist by that id. No global `thread_local` is used.

### 1.7.4 DEBUG is off on hot paths

Hot paths (per-frame parsing, per-I/O) must not log at DEBUG. In release builds the macro compiles away; in debug builds `Log::SetMinLevel(LogLevel::Info)` silences it at runtime.

### 1.7.5 Output policy (default)

| Sink | Default | Path / port | Rotation |
|---|---|---|---|
| console (stdout) | startup + production | — | none |

> No rolling files, syslog or EventLog: a single stdout sink is enough once systemd journal or container log collection is in place. Local rolling files are the deployer's job (logrotate / sidecar). ConsoleLogger keeps no sink registry.

### 1.7.6 Not-usable scenarios

1. INFO per frame/request -> disable or downgrade to DEBUG.
2. Literal passwords/tokens in log lines -> use `Error::Make(..., /*mask*/true)`.
3. Entire response byte dumps in the main log -> use a separate debug channel.
4. Assuming the ConsoleLogger singleton is lock-free -> it serialises internally with a mutex; coalesce high-frequency INFO lines.
5. Mismatched `{}` and argument counts -> no compile-time fmt check; excess arguments are truncated and missing ones raise `LogFormatError` (never swallowed silently).

---

## 1.8 ConfigLoader — staged pipeline with error aggregation

> The v1 loader lives in `src/Service/Config/JsonConfigLoader.cpp`. Three logical phases — **parse -> version gate -> field validation** — each with its own error code, implemented as five concrete stages.

### 1.8.1 Pipeline

```text
LoadConfig(jsonPath)
  |
  [stage 1: parse]
    - open file, read bytes
    - nlohmann::json parse
    - failure -> ConfigError("JSON parse failed: ...")
  |
  [stage 2: version gate (ADR-0005)]
    - read schemaVersion (root + every protocols/*.json)
    - missing / equal to kSupportedSchemaVersion (= 2) -> continue
    - < or > kSupportedSchemaVersion -> ConfigError
  |
  [stage 3: field-level validation]
    - ConfigValidator field by field:
        * type / required / usage matrix
        * reference relations (§1.2.4 matrix)
        * trial-parse expressions against the EBNF
    - **error aggregation**: no fail-fast; collect everything, report once
    - at least one error -> ConfigError
  |
  [stage 4: deserialization]
    - produce strongly typed POCOs
    - back-fill implicit fields (operation.name, protocol/device defaults)
  |
  [stage 5: second validation]
    - whole-ConfigRoot consistency at startup
    - protocol-device matrix coverage (every device has a protocol)
    - builtInFunctions match protocol features (auto requires an int variable)
```

### 1.8.2 Error aggregation (no fail-fast)

Stage 3 collects into `std::vector<ConfigErrorDetail>` and reports once:

```text
configuration validation failed (10 errors):
  [1] devices[2].connection.host: required field missing
  [2] devices[3].protocol: referenced protocol "ModbusTCP-v2" not defined under protocols/
  [3] devices[3].requestTimeoutMs: must be > 0
  [4] tags[10].operation: referenced operation "ReadCoil" not defined
  [5] tags[12].finalType: unsupported finalType "BCD" (supported: Bool/UInt16/...)
  [6] protocols/ModbusTCP.handshake[1].sessionVariable "SessionID" collides with device.password
  [7] protocols/ModbusTCP.operations.WriteCoil: write-named operation missing finalType=UInt16
  [8] operation ReadInputRegisters placeholder "{Crc:crc16modbus:X4}" uses the removed
      three-segment grammar; templates only allow {Name:Xn} and {Name:raw} (rule 6)
  [9] protocols/ModbusTCP.framing.type: "Raw" was removed (rule 5); see Config_Schema §8
  [10] root.resilience.cooldownMs: must be > 0
```

> Aggregation lets users see every configuration problem at once instead of fixing one error per run.

### 1.8.3 Error-code discipline

| Stage | Error code | Severity | Blocks startup |
|---|---|---|---|
| parse failure | `ConfigError` | fatal | yes |
| version gate failure | `ConfigError` | fatal | yes |
| missing field / wrong type | `ConfigError` | fatal | yes (after aggregation) |
| field warning (e.g. `operation` references a missing/removed tag) | (none) | warning | **no** (log warn) |
| orphan read operation | (none) | warning | **no** |
| protocol field mismatching the docs | (none) | warning | **no** |
| unknown `builtInFunctions` entry | `ConfigError` | fatal | yes |

### 1.8.4 hot-reload constraints

- The engine performs no in-place hot swap: `SIGHUP` triggers a **full restart**.
- Triggers: `SIGHUP` or `POST /api/admin/reload`.
- Restart order: save runtime state (LatestValueStore) -> stop WebApi -> stop PollingEngine -> stop ChannelManager -> reload config -> reinitialise.
- New connections are rejected during reload; a failed reload reverts to the previous configuration (runtime revert is on the [ROADMAP](../../ROADMAP.md) *zh*).

### 1.8.5 Not-usable scenarios

1. Editing the JSON while it is being parsed -> the loader records mtime and flags a reload hint afterwards.
2. Mutating POCO fields after load -> breaks the immutability contract and is not re-read on reload.
3. Skipping the version gate -> mismatched configurations still trigger field errors; the gate must run first.
4. Not resetting after load -> stale POCOs survive a reload; `ConfigRoot` must be RAII-owned.
5. Reading `nlohmann::json` nodes directly -> bypasses strong typing; always go through `JsonConfigLoader::ToPOCO`.
6. Concurrent loading from multiple threads -> the loader is **not** thread-safe; the application loads once on the main thread at startup.

---

## 1.9 Diagnostics: requestId correlation (ADR-0009)

`TagValue.requestId` (`int64_t`) is the end-to-end correlation id: assigned at `TagReader` entry, **shared** by all tags of one merged request, and propagated through build -> send -> parse -> dispatch (consumers reuse the value, never re-assign).

| Item | Value |
|---|---|
| Assignment point | `TagReader` entry |
| Merged request | all tags in the group share one `requestId` |
| Sentinel | `0` = "not traced"; business code must filter it out |

**Not-usable scenarios**

1. Re-using `requestId` across threads for unrelated work -> it is assigned at TagReader entry and only travels by value downstream.
2. Treating `requestId == 0` as a valid id -> it is a sentinel.

> Layered span tracing (`traceId` / `parentSpanId` / W3C Trace Context) is not implemented; see [ROADMAP](../../ROADMAP.md) *zh* and [ADR-0009](../../adr/0009-correlation-tracing.md) *zh*.

---

## 1.10 Type conversion matrix (inside `ResponseParser::Parse`)

> There is no standalone conversion utility: conversion happens inside [ResponseParser::Parse](./02_Engine.md), which calls `Core::FromBytesU16/U32/U64` according to `byteOrder` + `finalType`. This section defines the contract boundary.
> Input: `ByteView` (parsed raw bytes) + `finalType` string + `byteOrder`. Output: `Expected<TypedValue>` on success, `TypeConversionError` on failure.

### 1.10.1 Twelve finalTypes

| finalType | Expected bytes | Rule | Too few / out of range | NaN / invalid |
|---|---|---|---|---|
| `Bool` | 1 | `bytes[0] != 0` -> true | 0 bytes -> `TypeConversionError` | — |
| `UInt16` | 2 | `FromBytesU16(view, byteOrder)` | < 2 -> 0 (implicit) / `TypeConversionError` (strict mode) | — |
| `Int16` | 2 | `static_cast<int16_t>(FromBytesU16)` | same | — |
| `UInt32` | 4 | `FromBytesU32` | short -> 0 / `TypeConversionError` | — |
| `Int32` | 4 | `static_cast<int32_t>(FromBytesU32)` | same | — |
| `UInt64` | 8 | `FromBytesU64` | short -> 0 / `TypeConversionError` | — |
| `Int64` | 8 | `static_cast<int64_t>(FromBytesU64)` | same | — |
| `Float` | 4 | `FromBytesU32` -> `static_cast<float>` | short -> 0.0f | NaN / Inf pass through (quality stays Good) |
| `Double` | 8 | `FromBytesU64` -> `static_cast<double>` | short -> 0.0 | NaN / Inf pass through (Good) |
| `ByteArray` | 0..N | whole byte copy | empty is legal | bytes are legal by definition |
| `String` | 1..N | UTF-8 decode | decode failure -> `TypeConversionError` | — |
| `Empty` | 0 | (none) | any byte count is legal | — |

### 1.10.2 Implicit 0 vs explicit TypeConversionError

| Mode | Trigger | Behaviour | When enabled |
|---|---|---|---|
| implicit 0 | `ByteView` shorter than the expected width | silently returns 0 / 0.0 / false | default (matches §1.4.1) |
| explicit `TypeConversionError` | startup flag `--strict-conversion` | reports `TypeConversionError` (Uncertain quality) | **strongly recommended** in production |

### 1.10.3 NaN / Inf pass-through

For `finalType=Float/Double` the bit pattern is preserved:

```cpp
// v1 implementation inside ResponseParser::Parse
uint32_t u = FromBytesU32(view, BE);
float f;
memcpy(&f, &u, 4);  // NaN passes through
TypedValue v;
v.d = static_cast<double>(f);  // still NaN
v.type = ValueType::Float;
```

- NaN always counts as changed in the §1.3.6 rule (`NaN != NaN`, so every sample sets `valueChanged=true`), yet is still delivered with `Good` quality; upstream can use this for persistent-NaN alarms.
- Upstream consumers (MQTT) must handle NaN themselves (JSON `null` or the string `"NaN"`).

### 1.10.4 Byte-order fallback

`tag.byteOrder` -> protocol `dataByteOrder` -> `BigEndian` (see §1.4). `ByteArray` / `String` ignore byte order entirely.

### 1.10.5 Not-usable scenarios

1. `finalType=Float` with a byte count other than 4 -> `TypeConversionError` (never pad to 0.0f).
2. `finalType=String` with invalid UTF-8 -> `TypeConversionError` (never truncate at the first bad byte).
3. Negative array indices -> impossible; `finalType` is a string, not an index.
4. Calling `ResponseParser::Parse` after the bytes were moved -> the view dangles; copy into a vector first.
5. Type conversion on the hot path -> acceptable (O(N), N <= 8); never `dynamic_cast`.
6. Relying on implicit signed/unsigned `static_cast` -> always use the two-step cast described at the end of §1.4.

---

## 1.11 Thread-safety levels

> v1 asynchronous model: a **single `io_context` with a single-threaded `run()`** (`io.run_for` in `main.cpp`). Per-bus strands (P1 D) were withdrawn — with one io thread, handlers submitted in order on a channel execute in order, so `strand.post()` is a redundant indirection (see [ADR-0002 §6](../../adr/0002-transport-abstraction.md) *zh*).

| POCO | Level | Note |
|---|---|---|
| `Error` | **fully read-only** | immutable after construction; freely shareable |
| `Optional<T>` (hand-written) | **depends on T** | needs outer synchronisation if T is not thread-safe |
| `Expected<T>` | **depends on T** | embeds `Error`, which is thread-safe |
| `ByteView` | **fully read-only** | holds a const pointer; never outlives its buffer (§1.6.2) |
| `TypedValue` | **read-only after construction** | no mutex; ownership transfers to the consumer |
| `TagValue` | **read-only after construction** | same; consumers hold independent copies |
| `TagDefinition` | **read-only after startup** | immutable once loaded |
| `DeviceConfig` | **read-only after startup** | same |
| `ProtocolConfig` | **read-only after startup** | same |
| `OperationConfig` | **read-only after startup** | same |
| `HandshakeStep` | **read-only after startup** | same |
| `WebApiConfig` | **read-only after startup** | same |
| `ResilienceConfig` | **read-only after startup** | same |

### 1.11.1 Mutable containers

| Structure | Synchronisation | Location |
|---|---|---|
| `LatestValueStore` | `std::mutex` (same mutex for readers and writer; the critical section only touches the map) | `src/Polling/include/MyProt/Polling/LatestValueStore.hpp` |
| `ConnectionManager` | `asio::strand` (serialised per channel) | `src/Gateway/src/ChannelManager.cpp` |
| `CircuitBreaker` | per-device `std::mutex` | `src/Gateway/src/Resilience.cpp` |
| `WebApiServer` | independent strand per connection | `src/WebApi/src/WebApiServer.cpp` |
| `PollingEngine` | single-threaded `io_context` | `src/Polling/src/PollingEngine.cpp` |
| `TagReader._writingInFlight` | per-device `std::atomic<bool>` (P1 C, ADR-0011 §3.1; P1 D withdrawn, ADR-0011 §3.2) | `src/Gateway/src/TagReader.cpp` |

### 1.11.2 "const is the default contract"

POCO classes expose **no** setters; fields are final after construction. Modification means constructing a new object:

```cpp
// ✓ correct
TagValue v2 = v1;
v2.timestamp = 2000;

// ❌ wrong (no such API)
v1.setTimestamp(2000);
```

### 1.11.3 Two ways to cross threads

| Way | Use for | Cost |
|---|---|---|
| pass by value (copy) | small objects (`Error`, `TagValue` with <= 10 fields) | stack copy, no allocation |
| share via `shared_ptr` | large objects (`TypedValue.bytes` with many bytes) | refcounted, atomic ops |

Bare pointers and references must never cross threads (unclear lifetime).

### 1.11.4 Not-usable scenarios

1. Multiple threads writing the same POCO -> use a strand or a mutex.
2. `const_cast` on const fields -> compile warnings; use `mutable` for internal state only.
3. Passing a ByteView that points into a vector across threads -> vector invalidation; use `shared_ptr<vector<uint8_t>>`.
4. Putting locks inside POCOs -> POCOs are pure data; locks belong to the outer container.
5. Relying on compile-time thread-safety -> data races are not detectable at compile time; code review plus this thread model is the control.

---

## 1.12 Configuration generations (ADR-0005)

Configuration carries an integer generation number, `schemaVersion`: optional at the `ConfigRoot` top level (written into `tags.json` in the split layout) and optional at the top of each protocol file.

`kSupportedSchemaVersion = 2` — the field set with `inputs` / `outputs` grouping plus `derivedLength` (see [Config_Schema.md](../Config_Schema.md)).

**Gate semantics** (runs before field validation; the only check in this list that terminates early):

| Case | Result |
|---|---|
| missing | Warning + assume the current generation |
| present but not equal to `kSupportedSchemaVersion` | `ConfigError` fail-fast, field validation skipped |
| protocol file disagrees with the root | `ConfigError` |

**Evolution policy**: breaking format changes bump the generation. With multiple clients, generations are **"high wins, low fails"** — an older client reading a newer generation fails at the gate and must be upgraded first. A migration tool is not implemented yet ([ROADMAP](../../ROADMAP.md) *zh*).

**Not-usable scenarios**

1. `schemaVersion` inconsistent with the fields (an old generation carrying new fields) -> the gate rejects it.
2. Upgrading configuration without upgrading the binary -> the binary only knows its own generation; upgrade both.
3. Rolling back without keeping `.bak` files -> configuration loss; keep configs in version control.

See [ADR-0005](../../adr/0005-config-versioning.md) *zh*.

---

## 1.13 Section summary

> The eight cross-cutting concerns (§1.5-1.12) cover the whole "mechanism layer" of Core. Combined with Part I they describe:

| Layer | Sections | Contents |
|---|---|---|
| data | §1.1-1.4 | Error / Expected / Optional / Config POCO / TypedValue / TagValue / ByteOrder |
| mechanism | §1.5-1.12 | Optional contract / ByteView contract / logging / ConfigLoader / tracing / type conversion / concurrency / versioning |

**Cross-document references**

- Engine ([02_Engine](./02_Engine.md)) implements `ResponseParser::Parse` (including `FromBytesU16/U32/U64` assembly; the standalone `ConvertToFinalType` was withdrawn, item A2), `MiniExpression` and `RequestBuilder`.
- Transport ([03_Transport](./03_Transport.md)) implements `IChannel` dispatch.
- Gateway ([05_Gateway](./05_Gateway.md)) implements `TagReader` and the write path.
- WebApi ([07_WebApi](./07_WebApi.md)) implements the `Error -> HTTP` mapping.
- ADR-0005 and ADR-0009 connect at §1.8 and §1.9.
- [architecture/04](../architecture/04_Error_Shutdown_Security.md) connects at §1.7 and §1.11.

---

## 1.14 Related index

- **Not-yet-implemented extensions** (TLS / CAN / W3C Trace / RBAC / coroutines): [ROADMAP](../../ROADMAP.md) *zh*
- **Architecture decisions and rationale**: [adr/](../../adr/) *zh*
- **Single source of truth for the configuration contract**: [Config_Schema.md](../Config_Schema.md)

# MyProtV2 — Transport Module

> **Series**: MyProtV2 module design
> **Previous**: [Engine module](./02_Engine.md)
> **Next**: [Service module](./04_Service.md)

---

**Depends on**: `Core` + asio + OpenSSL

> **Authoritative sources**: the scope and abstraction decisions of the transport layer (v1 = TCP/TLS + serial/RTU; `Connect` endpoint generalization, framing classification, shared-bus semantics) follow [ADR-0002](../../adr/0002-transport-abstraction.md) *zh*; configuration fields follow [Config_Schema.md](../Config_Schema.md) §2.1/§2.2/§4. If this document conflicts with them, those two win.

## 3.1 IChannel

> **Async model** ([ADR-0010](../../adr/0010-vs2015-cpp11-toolchain.md) §2 *zh*): VS2015/C++11 has no coroutines, so every async operation follows the **trailing completion handler** convention - it returns `void` and delivers the result through a `std::function<void(Expected<T>)>` callback.

```cpp
// src/Transport/include/MyProt/Transport/IChannel.hpp

#include <chrono>
#include <functional>
#include <memory>

namespace MyProt { namespace Transport {

using Bytes = Core::Bytes;   // std::vector<uint8_t>

/// Connect completion callback (invoked exactly once, on success or failure)
using ConnectHandler = std::function<void(Core::Expected<void>)>;

/// Request-response completion callback (invoked exactly once when a complete
/// response frame arrives or on failure)
using ReceiveHandler = std::function<void(Core::Expected<Core::Bytes>)>;

/// One-way send completion callback (invoked exactly once). Success only means the bytes
/// were handed to the transport, not that the device received/processed them
/// (there is no way to confirm when the device does not answer).
using SendHandler = std::function<void(Core::Expected<void>)>;

/// Channel base class - the unified interface of the concrete Tcp/Tls/Serial channels.
/// One request per channel: only one SendReceive may be in flight at a time
/// (the gateway layer serializes per device).
class IChannel {
public:
    virtual ~IChannel() = default;

    /// Establish the physical connection asynchronously; a timeout before completion fails with Timeout
    virtual void Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) = 0;

    /// Async request-response: send request and collect a complete response frame per framingConfig;
    /// a null framingConfig uses the channel's built-in default framing
    virtual void SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                             ReceiveHandler handler) = 0;

    /// One-way send (fire and forget): completes as soon as request is written - no read,
    /// no framing, no waiting for a reply. Only for scenarios where the device really does
    /// not answer (e.g. Modbus broadcast); if the device does reply, the unread bytes remain
    /// buffered and misalign subsequent responses on the same channel.
    /// The default implementation returns NotImplemented; Tcp/Serial override it, the Tls stub does not.
    virtual void Send(const Bytes& request,
                      std::chrono::milliseconds timeout,
                      SendHandler handler) {
        (void)request; (void)timeout;
        handler(Core::Unexpected(Core::Error::Code::NotImplemented,
                                 "one-way send not supported by this channel"));
    }

    /// Disconnect and release underlying resources; pending operations complete with ConnectionClosed
    virtual void Disconnect() = 0;

    virtual bool IsConnected() const = 0;

    /// Snapshot of the last failure (thread-safe)
    virtual Core::Error GetLastError() const = 0;
};

/// Channel smart pointer
using IChannelPtr = std::shared_ptr<IChannel>;

}} // namespace MyProt::Transport
```

> **Callback thread**: handlers fire on the channel's `io_context` thread; implementations capture
> their operation state with `shared_ptr` to keep it alive (the callback chain holds it until
> completion). The current deployment is a single `io_context` with a single-threaded `run()`, so
> callbacks are naturally serial.
>
> **One request per channel**: `TcpChannel` keeps an internal `_inFlight` guard; a second concurrent
> `SendReceive` fails immediately with `Busy` (a socket does not support concurrent reads/writes).
>
> **Reconnect responsibility**: channels never reconnect internally; this is driven uniformly by
> `ChannelManager::GetOrCreateChannel` (with a 2 s entry cooldown). The breaker (`SessionContext`)
> provides pacing through `cooldownMs` (default 10000).

> **TCP auto-reconnect (implemented 2026-08-29, changed to one-way throttling 2026-08-31)**:
>
> | Trigger | Behaviour |
> |---------|------|
> | `Connect` succeeds | `_connected.store(true)`; the endpoint is not cached (reconnect responsibility moved up to ChannelManager) |
> | `SendReceive` entry sees `_connected=false` | Returns `ConnectionClosed` immediately and does **not** reconnect inside this method (avoids a double-Connect storm with ChannelManager's GOC) |
> | `SendReceive` fails (WriteFailed / ConnectionClosed / Timeout) | `_connected.store(false)` -> the next poll goes through the full reconnect chain in `ChannelManager.GetOrCreateChannel` (the entry has a 2 s cooldown) |
> | `Disconnect` (manual) | `_connected.store(false)` and close the socket; the next poll reconnects via `ChannelManager.GetOrCreateChannel` |
>
> **With keepalive** (see §3.1.1): the OS detects a peer RST within 2-6 s, the socket closes -> error path
> sets `_connected=false` -> the next polling cycle (default 1 s) reconnects naturally.

## 3.1.1 TCP keepalive

> Cross-platform headers [src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp](../../../src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp) + [NativeSocket.hpp](../../../src/Transport/include/MyProt/Transport/NativeSocket.hpp) - `SetSocketKeepAlive(NativeSocket, onoff, idleSec, intvlSec, cnt)`.
>
> - **Windows**: `SIO_KEEPALIVE_VALS` (ioctl, millisecond precision)
> - **Linux / BSD**: `setsockopt(TCP_KEEPIDLE/TCP_KEEPINTVL/TCP_KEEPCNT)` (second precision)
>
> v1 defaults: `idle=2s`, `intvl=2s`, `cnt=3` (a peer close is detected within roughly 6 s).
>
> Trigger chain: simulator/PLC closes -> the OS detects the RST within 2-6 s -> `socket.read` returns an
> error at once -> `SendReceive` failure callback -> `_connected.store(false)` -> the next `SendReceive`
> entry triggers a reconnect (through ChannelManager).
>
> Call site: after a successful `TcpChannel::Connect`, `set_option(no_delay)` is immediately followed by
> `SetSocketKeepAlive(socket.native_handle(), true, 2, 2, 3)`.

## 3.1.2 Simulation-side config source (legacy)

`SimulationServer`'s `listenPort` / `registerCount` / `initialValues` / `operations` configuration came
from the `ServerConfig.simulation` section of `configs/server.json` (the protocol layer carried no
server-side behaviour).

- Old: the `ProtocolConfig.simulation` field; `RuntimeGlue` started simulation by iterating `loaded.protocols`
- New: `ServerConfig.simulation`; `RuntimeGlue` starts simulation when `loaded.server.simulation.listenPort > 0`, with several protocols sharing the same endpoint

> **Note**: this section is legacy. The current implementation is protocol-scoped simulation instances
> (`ProtocolConfig.simulation`, ADR-0017); `server.json` is only a fallback when no protocol enables
> simulation. See [10_Simulation §10.1](./10_Simulation.md) and [ADR-0017](../../adr/0017-protocol-scoped-simulation-instances.md) *zh*.

## 3.1.3 UDP datagram channel (UdpChannel)

[src/Transport/include/MyProt/Transport/UdpChannel.hpp](../../../src/Transport/include/MyProt/Transport/UdpChannel.hpp) + [src/Transport/src/UdpChannel.cpp](../../../src/Transport/src/UdpChannel.cpp) - the `IChannel` implementation of `TransportType::Udp` (ADR-0002 §7). Key differences from TCP:

| Dimension | TcpChannel | UdpChannel |
|------|-----------|-----------|
| `Connect` | real async TCP connect | only `open` the socket and resolve the peer endpoint; success sets `_connected=true` |
| Response boundary | cut frames inside a stream per `framing` (`Measure` / `FrameComplete`), keep calling `ReadChunk` until complete | **one datagram = one frame**: `async_receive_from` returns a datagram whole, never concatenated |
| `framing` role | decides frame boundaries | only `maxFrameSize` is used, as the receive buffer cap (degraded) |
| Timeout/error | close socket + `_connected=false` -> ChannelManager rebuilds | same (close also drops late datagrams so they cannot mismatch the next response) |
| Single in-flight | `_inFlight` guard -> concurrent `SendReceive` returns `Busy` | same |

- `Send` (one-way/broadcast): `async_send_to` completes once sent, with no read-back.
- Endpoint cache key: `udp:host:port` (`ChannelManager::ComputeEndpointKey`); when a device has `port=0` it falls back to the protocol's `transport.udp.defaultPort`.
- The channel factory (`App/main.cpp`) creates a `UdpChannel` for `TransportType::Udp`.
- Unit tests: [tests/Transport.Tests/UdpChannelTests.cpp](../../../tests/Transport.Tests/UdpChannelTests.cpp) use asio loopback to cover echo framing, no-reply timeout and single-in-flight Busy.

## 3.2 IFrameParser

```cpp
// src/Transport/include/MyProt/Transport/IFrameParser.hpp

namespace MyProt { namespace Transport {

/// Frame parsing result
struct FrameParseResult {
    std::vector<uint8_t> frame;   // complete frame bytes
    size_t consumedBytes;         // number of bytes consumed
    bool needMoreData;            // whether more data is required
};

/// Frame parser interface
class IFrameParser {
public:
    virtual ~IFrameParser() = default;

    /// Try to parse a frame from the buffer (data[startPos..])
    virtual Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) = 0;

    /// Check whether the buffer starts with a complete frame
    virtual bool HasCompleteFrame(const ByteView& data) = 0;
};

using IFrameParserPtr = std::shared_ptr<IFrameParser>;

}} // namespace MyProt::Transport
```

> **The four framing classes and where they live** (see [ADR-0002](../../adr/0002-transport-abstraction.md) *zh*):
>
> | type | Framing basis | Implementation | v1 |
> |------|------|------|:--:|
> | `LengthField` | length field in the frame header (fixed width or varint) | `LengthFieldFrameParser` (`Parse` / `Measure`); shared by `TcpChannel::FrameComplete`, the simulator and `StreamFrameCodec` | yes |
> | `Fixed` | fixed byte count | **no dedicated parser class** (see §3.3); `TcpChannel::FrameComplete` and `SimulationServer` decide inline on `fixedLength` | yes |
> | `Silence` | inter-character silence timeout (RTU) | `SerialChannel` read loop (`frameGapUs`); on TCP it degrades to "the first data block is one frame" | yes |
> | `Message` | one read is one frame (CAN 8/64B) | channels send/receive whole frames, no stream reassembly | v1 validator reports a name error |
>
> Current state: `IFrameParser` has exactly one implementation, `LengthFieldFrameParser`, and the
> length-field semantics (fixed / varint / `lengthAdjustment`) exist only inside it:
>
> - the simulator `SimulationServer` calls `Parse` directly to cut frames;
> - the polling path `TcpChannel::FrameComplete` calls `LengthFieldFrameParser::Measure` (measures frame boundaries only, no copy);
> - the ingest path `StreamFrameCodec` (`Core::IFrameCodec`) builds a `Core::Frame` on top of `Parse` and performs bit-field/dispatch/record extraction ([ADR-0021](../../adr/0021-config-driven-framing-primitives.md) *zh*).
>
> MQTT has its own `IFrameCodec` implementation (`Northbound::MqttCodec`) and does not reuse
> `StreamFrameCodec` - but MQTT's varint semantics are protocol-defined, not derived from framing config.

## 3.3 Fixed framing (no dedicated parser)

v1 has **no** `FixedFrameParser` class (early design A5, withdrawn). Fixed-length framing is implemented inline in two places:

- `TcpChannel` (`FrameComplete` in `src/Transport/src/TcpChannel.cpp`): `b.size() >= framing.fixed.fixedLength` counts as a complete frame; the frame length cap is `fixedLength`.
- `SimulationServer` (simulator frame cutting): slices fixed-length frames directly by `_protocol.framing.fixed.fixedLength`.

Config fields are in [Config_Schema.md](../Config_Schema.md) §2.2 (`framing.type="Fixed"` plus `fixedLength > 0`, rule 4).

## 3.4 LengthFieldFrameParser

```cpp
// src/Transport/include/MyProt/Transport/LengthFieldFrameParser.hpp

namespace MyProt { namespace Transport {

/// LengthField frame parser implementation
class LengthFieldFrameParser : public IFrameParser {
public:
    /// Frame boundary measurement result (no frame copy); shared by Parse and TcpChannel::FrameComplete
    struct FrameLayout {
        bool needMore;
        bool malformed;
        std::size_t totalBytes;
        std::string error;
    };
    static FrameLayout Measure(const Core::LengthFieldConfig& config,
                               const uint8_t* data, std::size_t available);

    explicit LengthFieldFrameParser(const Core::LengthFieldConfig& config);

    // IFrameParser implementation
    Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) override;
    bool HasCompleteFrame(const ByteView& data) override;

private:
    Core::LengthFieldConfig _config;
    enum class LengthStatus { Complete, NeedMore, Malformed };

    static LengthStatus ReadLength(const Core::LengthFieldConfig& config,
                                   const uint8_t* frame, size_t available,
                                   size_t& value, size_t& width, std::string& err);
    static int64_t CalculateTotalFrameSize(const Core::LengthFieldConfig& config,
                                           size_t bodyLength, size_t lengthWidth);
    static bool ValidateFrameSize(const Core::LengthFieldConfig& config, int64_t totalSize);

    static uint16_t ToUInt16BigEndian(const uint8_t* data);
    static uint16_t ToUInt16LittleEndian(const uint8_t* data);
    static uint32_t ToUInt32BigEndian(const uint8_t* data);
    static uint32_t ToUInt32LittleEndian(const uint8_t* data);
};

}} // namespace MyProt::Transport
```

**Frame-size formula** (`LengthFieldFrameParser::CalculateTotalFrameSize`):

- `lengthIncludesHeader=true` -> `total = lengthValue + lengthAdjustment`
- `lengthIncludesHeader=false` -> `total = headerLength + lengthValue + lengthAdjustment`; when `headerLength = 0` and `lengthFieldEncoding=Varint`, `headerLength` is treated as `lengthFieldOffset + actual varint width`
- `ValidateFrameSize`: `0 < total <= maxFrameSize`
- Fixed-width encoding: `lengthFieldLength` is one of `{1,2,4}`, with byte order from `byteOrder` (`ExtractLengthValue`)
- `Varint` ([ADR-0021](../../adr/0021-config-driven-framing-primitives.md) §2.1 *zh*): `lengthFieldLength` must be 0 and `varintMaxBytes` must be within `[1,4]`; base-128 (low 7 bits payload, high bit continues), the width comes from the encoding itself; still unterminated beyond the cap is illegal
- Insufficient data returns `FrameParseResult{ needMoreData = true }` and the caller keeps accumulating

`Measure` and `Parse` share the same length-field/frame-size computation, but `Measure` **does not copy
the frame**: it only returns `{needMore, malformed, totalBytes}` for hot paths such as
`TcpChannel::FrameComplete` that only care whether a full frame is available.

## 3.4.1 Config-driven frame specification (ADR-0021)

`StreamFrameCodec` is the bottom of the frame stack (`Core::IFrameCodec`); it turns a `LengthField` /
`Fixed` byte stream into a field-carrying `Core::Frame`:

| Primitive | Config | Behaviour |
|------|------|------|
| varint length field | `lengthFieldEncoding=Varint` + `varintMaxBytes` | frame cutting; the header width is computed dynamically |
| bit-field extraction | `headerFields[{name,bitOffset,bitWidth}]` | counted from the frame's most significant bit, written as `UInt` fields of `Frame` |
| dispatch | `dispatch{field,cases[],rejectUnknown}` | a hit sets `Frame::type` / `typeName`; a miss marks the frame illegal by default |
| repeated records | `repeat{countField,stride,offset,name,maxCount}` | `[offset, offset+count*stride)` inside the frame becomes a `Bytes` field |

Boundary semantics:

- All bit offsets and record offsets are relative to the **frame start** (header included), not to the payload.
- For `LengthField`, `Frame::payload` starts after the frame header (`headerLength`, or `lengthFieldOffset + actual length-field width`); for `Fixed`, the payload is the whole frame.
- Specification violations uniformly return `Malformed` (an ingest caller drops the connection); there is no "best-effort parsing".
- Load-time validation (`SchemaChecker`) covers: varint width, bit-field range/duplicate names, dispatch field existence and duplicate case values, and repeat stride/offset/maxCount.
- This layer interprets no field semantics and owns no session state; CRC/checksum, variable-length records and HTTP chunked are not covered.

## 3.5 TcpChannel

```cpp
// src/Transport/include/MyProt/Transport/TcpChannel.hpp

namespace MyProt { namespace Transport {

/// TCP channel - built on asio::ip::tcp::socket
/// Timeouts use a steady_timer watchdog: on expiry the socket is closed and pending
/// operations finish with operation_aborted
class TcpChannel : public IChannel,
                   public std::enable_shared_from_this<TcpChannel> {
public:
    TcpChannel(asio::io_context& io, const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// Receive operation state (kept alive by the callback chain)
    struct RxOp {
        ReceiveHandler handler;
        Core::Bytes buffer;
        std::shared_ptr<const Core::FramingConfig> framing;
        std::shared_ptr<asio::steady_timer> watchdog;
        size_t maxFrameSize;
        bool done;                    // timer vs I/O race -> the handler fires exactly once
    };

    /// Append one data chunk; after each arrival the framing rules decide completeness
    void ReadChunk(const std::shared_ptr<RxOp>& op);

    /// Record the last error (thread-safe)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    std::string _channelId;
    std::unique_ptr<asio::ip::tcp::socket> _socket;
    asio::ip::tcp::resolver _resolver;
    std::atomic<bool> _connected;
    std::atomic<bool> _inFlight;      // one request per channel (the second fails with Busy immediately)
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

> **Framing**: after `SendReceive`, `ReadChunk` keeps calling `async_read_some`; each arriving chunk is
> judged by `framingConfig` (`Fixed` / `LengthField` / `Silence` branches as in the §3.2 table);
> `Silence` on TCP degrades to "the first data block is the complete response". The frame-size cap is
> taken per framing branch (Fixed -> `fixedLength`, Silence -> `silence.maxFrameSize`, otherwise
> `lengthField.maxFrameSize`).
> **Watchdog**: an expiring `RxOp::watchdog` closes the socket and completes with `Timeout`; every
> completion path calls `cancel()` on the timer, and `RxOp::done` guarantees the handler fires exactly once.

## 3.6 TlsChannel

v1 is a **stub: the signature is frozen but it is not implemented** - `Connect` / `SendReceive` fail
immediately with `NotImplemented` and `IsConnected()` returns false. The validator emits a Warning for
`transport.type="Tls"` (the runtime channel does not exist yet).

```cpp
// src/Transport/include/MyProt/Transport/TlsChannel.hpp

namespace MyProt { namespace Transport {

/// TLS channel - based on asio::ssl::stream<asio::ip::tcp::socket> (to be implemented)
class TlsChannel : public IChannel,
                   public std::enable_shared_from_this<TlsChannel> {
public:
    TlsChannel(asio::io_context& io,
               const Core::TlsTransportConfig& tlsConfig,
               const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    asio::io_context& _io;
    Core::TlsTransportConfig _tls;
    std::string _channelId;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

## 3.7 SerialChannel

```cpp
// src/Transport/include/MyProt/Transport/SerialChannel.hpp

namespace MyProt { namespace Transport {

/// Serial channel - based on asio::serial_port
/// Silence framing: a gap between characters longer than frameGap ends the frame
class SerialChannel : public IChannel,
                      public std::enable_shared_from_this<SerialChannel> {
public:
    SerialChannel(asio::io_context& io,
                  const Core::SerialTransportConfig& serialConfig,
                  const std::string& channelId);

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;
    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;
    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// Silence-framing receive state (accumulating buffer + gapTimer + watchdog, kept alive by the chain)
    struct OpState;

    /// Append one character chunk; every arrival restarts the silence timer
    void ReadSome(const std::shared_ptr<OpState>& op,
                  const std::shared_ptr<asio::serial_port>& port);

    /// Record the last error (thread-safe)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    Core::SerialTransportConfig _serial;
    std::string _channelId;
    std::shared_ptr<asio::serial_port> _port;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;
};

}} // namespace MyProt::Transport
```

**Framing behaviour**: `SendReceive` requires `framingConfig` to be the `Silence` branch; `frameGapUs`
**must be configured explicitly** (`<= 0` fails at once with `ConfigError`; there is no "3.5 x single
character time" default and no Modbus RTU fallback). After writing the request a silence window starts:
no byte arriving inside the window -> `Timeout`; bytes keep arriving -> `ReadSome` continues and each
arrival restarts the silence timer until the silence exceeds `frameGapUs`, which completes the frame.

**Character-time conversion**: the single character time is
`charTimeUs = (dataBits + stopBits + (parity?1:0) + start bit) * 1e6 / baudRate` and is diagnostic only
(when `charTimeUs=0` the validator requires a protocol-level `baudRate > 0` to derive it). `Silence`
framing **does not go through** `IFrameParser`.

**Connection state reset**: a watchdog timeout or read error is a connection-level fault -
`_connected.store(false)` + `_port.reset()`, so `IsConnected()` turns false and the next attempt goes
through `ChannelManager::GetOrCreateChannel` for reconnect. Before the fix only `close()` was called
without resetting the flag, so `IsConnected()` stayed true forever and the device was permanently dead.

**CanChannel**: not implemented (no implementation in the repository); if enabled, see
[ROADMAP.md](../../ROADMAP.md) and the design notes in [ADR-0002](../../adr/0002-transport-abstraction.md)
§4 *zh* - it presents a pseudo byte stream of `[CAN ID big-endian][payload]` to the engine, so `resp[]`
and `requestTemplate` can reference the CAN ID directly with zero engine changes.

---

> Un-implemented extensions are tracked in [ROADMAP.md](../../ROADMAP.md).
> **Previous**: [Engine module](./02_Engine.md)
> **Next**: [Service module](./04_Service.md)

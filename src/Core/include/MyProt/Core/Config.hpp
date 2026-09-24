// src/Core/include/MyProt/Core/Config.hpp
// Config POCO - strongly-typed config via tagged structs (C++11, ADR-0010 §3)
// Replaces std::variant; the discriminated union is expressed with a type enum + flat fields

#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include "MyProt/Core/Optional.hpp"    // in-house Core::Optional<T> (C++11 compatible, replaces std::optional; rolled back 2026-08-29 as v140 lacks <optional>)

#include "MyProt/Core/ByteOrder.hpp"   // LengthFieldConfig depends on the ByteOrder enum
#include "MyProt/Core/SimulationConfig.hpp" // ServerConfig / referrers (ConfigDeepValidator) still need this header

namespace MyProt { namespace Core {

// ────────── framing config (discriminated union) ──────────

struct LengthFieldConfig {
    int lengthFieldOffset;          // byte offset of the length field within the frame (>=0, typically 2-4)
    int lengthFieldLength;          // length-field byte count (1/2/4)
    bool lengthIncludesHeader;      // true: the length value includes header bytes; false: body only
    ByteOrder byteOrder;            // length-field byte order (Modbus=BE, S7=LE, ...)
    int headerLength;               // fixed header byte count (used to locate the body start)
    int lengthAdjustment;           // bodyLen = parsedLen + lengthAdjustment
                                    // (used when the protocol frame length != actual data length, e.g. trailing CRC)
    int maxFrameSize;               // runtime frame-length safety cap (default 1024, guards against oversized frames)

    LengthFieldConfig()
        : lengthFieldOffset(0), lengthFieldLength(0), lengthIncludesHeader(false)
        , byteOrder(ByteOrder::BigEndian), headerLength(0), lengthAdjustment(0)
        , maxFrameSize(1024) {}
};

struct FixedConfig {
    int fixedLength;
    FixedConfig() : fixedLength(0) {}
};

struct SilenceConfig {               // serial/RTU silence framing (v1; framing is driven by the channel read loop)
    int charTimeUs;                  // single-character time (microseconds); 0 = auto-derived from the protocol-level baud rate/data bits (diagnostic reference only)
        int frameGapUs;                  // inter-frame silence threshold (microseconds); must be explicitly configured > 0 - the engine does not bake in protocol conventions and does not auto-derive it from charTimeUs
    int maxFrameSize;

    SilenceConfig() : charTimeUs(0), frameGapUs(0), maxFrameSize(256) {}
};

struct MessageConfig {               // CAN message framing (reserved; the v1 validator reports a name error)
    int maxFrameSize;                // classic CAN 8 / CAN FD 64
    int idFieldLength;               // bytes the CAN ID occupies in the pseudo byte-stream header

    MessageConfig() : maxFrameSize(8), idFieldLength(2) {}
};

enum class FramingType { LengthField, Fixed, Silence, Message };  // maps to the JSON discriminator field "type"

/// Framing config - tagged struct (replaces std::variant, ADR-0010 §3);
/// type selects the active branch; fields of an inactive branch must not be read.
struct FramingConfig {
    FramingType type;
    LengthFieldConfig lengthField;   // valid when type == LengthField
    FixedConfig fixed;               // valid when type == Fixed
    SilenceConfig silence;           // valid when type == Silence
    MessageConfig message;           // valid when type == Message (reserved)

    FramingConfig() : type(FramingType::LengthField) {}
};

// ────────── transport config ──────────

struct TcpTransportConfig {
    uint16_t defaultPort;         // consistent with the adl_serializer default (Modbus TCP)
    TcpTransportConfig() : defaultPort(502) {}
};

struct TlsTransportConfig {
    uint16_t defaultPort;
    std::string certFile;
    std::string keyFile;
    std::string caFile;
    bool verifyServer;

    TlsTransportConfig() : defaultPort(0), verifyServer(true) {}
};

struct SerialTransportConfig {
    std::string portName;    // COM1, /dev/ttyUSB0
    uint32_t baudRate;
    uint8_t dataBits;
    enum class Parity { None, Odd, Even };
    Parity parity;
    enum class StopBits { One, Two };
    StopBits stopBits;

    SerialTransportConfig() : baudRate(9600), dataBits(8), parity(Parity::None), stopBits(StopBits::One) {}
};

enum class TransportType { Tcp, Tls, Serial };  // maps to the JSON discriminator field "type"; CAN is reserved and not modeled (the v1 validator reports a name error)

/// Transport config - tagged struct (replaces std::variant, ADR-0010 §3);
/// type selects the active branch; fields of an inactive branch must not be read.
struct TransportConfig {
    TransportType type;
    TcpTransportConfig tcp;          // valid when type == Tcp
    TlsTransportConfig tls;          // valid when type == Tls
    SerialTransportConfig serial;    // valid when type == Serial

    TransportConfig() : type(TransportType::Tcp) {}
};

// ────────── response-parser config ──────────

/// The parser emits raw bytes only; the final type conversion is decided by tag.finalType (see Config_Schema §6)
struct ResponseParserConfig {
    std::string validCondition;        // e.g. "resp[1]==0x03"; empty = skip validation
    int dataStartIndex;
    std::string dataLengthExpr;        // e.g. "resp[2]"; empty = frame length - dataStartIndex

    ResponseParserConfig() : dataStartIndex(0) {}
};

// ────────── operation config ──────────

struct VariableConfig;   // forward declaration - OperationConfig::variables references it (full definition in the next section)

struct OperationConfig {
    std::string name;                  // backfilled by the loader from the operations map key; not written in JSON
    std::vector<std::string> requestTemplate;
    ResponseParserConfig responseParser;
        // operation semantic annotation (optional, empty = unannotated) - "read" | "write".
    //   Not depended on at runtime (the write path distinguishes by reference); used for UI form filtering and config validation
    //   (a tag's operation should be a read-class one, writeOperation a write-class one).
    std::string kind;
        // input/output parameter grouping (inputs / outputs) - there is no single variables section in the config:
    //   inputs : inputs provided by the operator / generated at runtime ( source=static value or pure UI hint
    //            + source=auto{autoIncrement,frameSlice,expr,crc} ). Merged at the same level into
    //            RequestBuilder's flat variable pool for template {Name:Xn} lookup-based rendering.
    //   outputs: derived outputs computed from inputs ( source=auto strategy=derivedLength, expr references
    //            "inputs ∪ {payload,count}" ). The parameter layer pre-parses by evaluating expr and injects into the variable pool; templates only look up.
    //   - within the same operation scope, inputs and outputs must not share a name (validation Error, preventing "one name leaking across").
    //   - role changes across operations are allowed: the same variable name can be an input or an output in different operations
    //     (e.g. RegisterCount: read= input / write= output), legal and each declaration is visible.
        //   - neither op.variables nor op.placeholderHints is in the current Schema (generation changes: see ADR-0005).
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;

    // Note: the single request-response timeout is configured uniformly via device.requestTimeoutMs (converged 2026-08-24);
    //       protocol operations no longer hold a timeoutMs - avoiding timeout drift across protocol/device.
};

// ────────── handshake steps ──────────

struct HandshakeStep {
    std::string name;
    std::vector<std::string> requestTemplate;
    Optional<FramingConfig> framingOverride;   // this step's own framing (JSON null/absent = use the channel default)
    std::string validCondition;                     // success-condition expression
    std::string sessionExtractExpr;                 // session-variable extraction expression e.g. "resp[5:9]"
    std::string sessionVariable;                    // variable name the extraction is stored under e.g. "SessionID"
    int timeoutMs;                                  // 0 = inherit device.connection.timeoutMs

    HandshakeStep() : timeoutMs(0) {}
};

// ────────── protocol variable declarations (inputs / outputs, two groups) ──────────

/// Unified protocol-variable declaration - a variable declares, in one place, its source + value/strategy + display meta info.
/// Replaces the old three sections (defaultVariables / autoComputeJson / metadata.placeholderHints).
//
//   source semantics (only two values; any other is a hard error):
//     "static" - declaration. With value -> injected at runtime into ctx.variables (a same-named key in the tag variables overrides it);
//                without value -> pure display meta info (not into ctx.variables, only renders a UI hint).
//     "auto"   - auto-computed. Templates {Name:Xn}/{Name:raw} consume its "parameter-layer resolved value"
//                (computation pushed down; no :auto:/:calc: tokens inside templates)
//
//   strategy (when source=auto):
//     autoIncrement / frameSlice / expr / crc - evaluated automatically on invocation (folded into autoComputeJson to feed AutoComputeProvider)
//     derivedLength - derived length: the parameter layer pre-parses it as a pure function of the payload byte count (payload),
//       using only expr (e.g. "{WriteValue:len} + 7" / "{WriteValue:len} * 8"); replaces the old hardcoded name families (PayloadPlus4/
//       PayloadBits/FrameWithUnit etc. removed together with the kind field).
//
//   source must be declared explicitly. auto requires strategy; static requires value (uint32_t or string -> at runtime by size).
//
//   A variable-length write payload needs no special strategy declaration - the engine scans the template for {Name:raw} placeholders to
//   auto-detect the payload, injecting the actual payload byte count into the {name:len} resolution table. The payload name in inputs is
//   for UI display only (source=static without value).
//
// JSON shape (option A: parameter layering, computation pushed down):
//   "inputs": {
//     "UnitID":        { "source": "static", "value": 1, "label": "slave address", "enum": [1..10] },
//     "TransactionID": { "source": "auto",   "strategy": "autoIncrement", "params": { "seed": 1 } }
//   },
//   "operations": {
//     "WriteMultipleRegisters": {
//       "inputs":  { "StartAddress": { "source": "static", "label": "start register address" },
//                    "WriteValue":   { "source": "static", "label": "write payload" } },  // UI display only; the payload is detected by template {WriteValue:raw}
//       "outputs": { "PDULength": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7", "label": "PDU length" },
//                    "ByteCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}", "label": "data byte count" } }
//     }
//   }
struct VariableConfig {
    std::string source;        // "static" | "auto"
    // static section: with value -> injected into ctx.variables at runtime; without value -> pure UI display
    Optional<uint32_t> value;  // optional when source=static (injected only when a value is present)
    // auto section
    std::string strategy;      // required when source=auto: autoIncrement|frameSlice|expr|crc|derivedLength
    std::string paramsJson;    // valid when source=auto (derivedLength does not use it): strategy params (consumed by AutoComputeProvider)
        // source=auto + strategy=derivedLength (sole expression):
    //   expr: a lightweight arithmetic expression over the payload byte count (e.g. "{WriteValue:len} + 7", "{WriteValue:len} * 8");
    //   {name:len} references the byte length of a template variable (raw payload = actual byte count, others = template render width);
    //   no reliance on built-in nouns - fully self-explanatory by formula.
    std::string expr;
    // display meta info (common to all source values)
    std::string label;         // display name (e.g. "slave address")
    std::string unit;          // unit (e.g. "bytes", "Hz", "count")
    /// Enum candidate members: value = the value actually used in rendering, label = display text.
    ///   JSON shape (Config_Schema §3.2): an element may be a bare number (shorthand, label takes the decimal literal)
    ///   or a { "value": N, "label": "..." } object; mixing is legal.
    struct EnumMember {
        uint32_t value;
        std::string label;
        EnumMember() : value(0) {}
    };
    std::vector<EnumMember> enumValues;   // optional enum candidates (UI dropdown; empty = do not switch input form)
    std::string placeholder;   // placeholder hint (UI input-box placeholder)

    bool isStatic() const { return source == "static"; }
    bool isAuto()   const { return source == "auto"; }
    bool isDerivedLength() const { return isAuto() && strategy == "derivedLength"; }
};

/// Simulation config (optional protocol-level section) ──────────

/// See SimulationConfig.hpp for the simulation operation behavior description.

/// Protocol-level simulation config; simulation lives at the top level of server.json under ServerConfig.simulation.
/// The protocol layer is fully decoupled from server-behavior fields such as listenPort / initialValues / packetLossRate.

/// ────────── config generation ──────────

/// Supported config generation number (ADR-0005 version gate) - **single source of truth**.
///   The schemaVersion of all three - protocol file / tags.json root / server.json - must exactly equal this value
///   (absent = Warning + assume this value; mismatch = ConfigError Fail-Fast).
/// This value's sole source is here; assembly points (main.cpp / RuntimeGlue.cpp /
///   ConfigStoreOptions defaults), and a Core comment referenced a never-defined constant
///   kSupportedSchemaVersion - now the constant is provided and referenced uniformly.
/// Changing this value requires migrating every config under configs/ (the ADR-0005 migration flow).
const int kSupportedSchemaVersion = 2;

/// ────────── protocol config ──────────

/// Default maximum byte span for address-proximity tag coalescing (consumed by TagGrouper::CoalesceAdjacent).
///   This is the general expression of a protocol family's "single-read cap": Modbus FC03 caps at 125 registers = 250 bytes.
///   Semantics: the early hardcoded 125 meant "125 registers"; after the address unit changed to bytes
///   (StartByteAddress / ByteCount) the value was not adjusted in sync, effectively leaving only 125 bytes
///   (=62 registers), otherwise batch-read coalescing capacity would silently halve. It is now the protocol-level maxSpanBytes
///   with the default corrected (250 = 125 registers x 2 bytes).
const int kDefaultMaxSpanBytes = 250;

struct ProtocolConfig {
    std::string protocolName;
    TransportConfig transport;
    FramingConfig framing;
    Optional<ByteOrder> dataByteOrder; // protocol-level data-decode byte order; falls back here when a tag does not explicitly declare byteOrder; unset = BigEndian
    std::unordered_map<std::string, OperationConfig> operations;
    std::vector<HandshakeStep> handshake;             // empty array = no handshake (Modbus)
        // protocol-level writeOperation / writeBytesOperation do not belong to the Schema -
    //   write capability is declared only at the tag layer (tag-level writeOperation / writeBytesOperation /
    //   direction=write, three forms); protocol-level fields once made read-only tags implicitly writable (legacy fallback),
        //   contradicting "config-as-contract" (the sole write-declaration point: the tag).
        // The simulation block lives in ServerConfig (server.json); the protocol layer carries no server behavior.
        // Variable declarations normalize into two groups, inputs / outputs (no defaultVariables/autoComputeJson/placeholderHints sections).
        // inputs / outputs split by source:
    //     - inputs.source=static with value -> injected into the flat variable pool (old static semantics); without value -> UI only (former hint)
    //     - inputs.source=auto          -> assembled into autoComputeJson to feed AutoComputeProvider (non-derivedLength)
    //     - outputs.source=auto derivedLength -> the parameter layer pre-parses by evaluating expr and injects (references template variable names, {name:len} takes byte length)
        //     - variable-length write payload: detected automatically from template {Name:raw} placeholders, no special inputs declaration needed
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;
    int schemaVersion;                        // config generation number (ADR-0005); must equal kSupportedSchemaVersion
        // variable alias map (alias -> internal name).
    //   Users customize variable names in the protocol JSON's variableAliases section; the engine still uses contract names internally.
        //   Only 2 mapping targets exist - StartByteAddress / ByteCount (the cross-protocol byte units the engine actually looks up);
    //     other legacy names have been demoted/removed from the contract, see Config_Schema.md §2.
    //   Note: a global variable-aliases.json file is not yet implemented - the loader only reads this section within protocol files.
    //   Aliases are normalized once at load time by ConfigDirectoryLoader::ApplyVariableAliases,
    //   so runtime consumers need not be aware; this map is only a fallback for the direct-construction path (bypassing the loader).
    std::unordered_map<std::string, std::string> varAliasMap;
        // maximum byte span for address-proximity tag coalescing (semantics: see kDefaultMaxSpanBytes).
    //   The protocol family's "single-read cap" is expressed uniformly here; the engine no longer bakes in any concrete number.
    //   Note: v1 does not distinguish read-operation types (Modbus FC01 coil cap 2000 far exceeds FC03 register cap 125),
    //       so a protocol author should set it to the tightest read-operation class.
    int maxSpanBytes;

    ProtocolConfig() : schemaVersion(kSupportedSchemaVersion)
                     , maxSpanBytes(kDefaultMaxSpanBytes) {}

    /// Resolve a user-defined name to the engine-internal name; returns the original name when there is no alias.
    std::string ResolveVariableName(const std::string& name) const {
        auto it = varAliasMap.find(name);
        if (it != varAliasMap.end()) return it->second;
        return name;
    }
};

// ────────── device config ──────────

/// Connection parameters (flat struct; fields are used per the protocol transport type; applicability validation: see Config_Schema §7)
struct ConnectionConfig {
    std::string host;              // Tcp/Tls: hostname or IP
    uint16_t port;                 // Tcp/Tls: 0 = use the protocol defaultPort
    std::string portName;          // Serial: overrides the protocol-level portName
    int timeoutMs;                 // connection-establishment timeout

    ConnectionConfig() : port(0), timeoutMs(3000) {}
};

/// Resilience policy (Config_Schema §11 / ADR-0004). The global block and the device-level override are isomorphic; an omitted device-level field falls back to the global value.
struct ResilienceConfig {
    int maxAttempts;           // total read attempts (incl. the first); writes are always 1
    int backoffBaseMs;         // exponential-backoff base
    int backoffMaxMs;          // single-backoff cap
    int failureThreshold;      // consecutive logical-read failures (budget exhausted) -> breaker opens
    int cooldownMs;            // breaker-open duration; during it, CircuitOpen fails fast
    int halfOpenProbes;        // half-open probe count

    ResilienceConfig()
        : maxAttempts(3), backoffBaseMs(100), backoffMaxMs(1000)
        , failureThreshold(5), cooldownMs(10000), halfOpenProbes(1) {}
};

/// Management-plane security (Config_Schema §12 / ADR-0008). An optional top-level block in ConfigRoot; nullopt = all defaults.
struct WebApiConfig {
    std::string bindAddress;       // listen address; loopback-only by default, remote management requires explicitly setting 0.0.0.0
    std::string certFile;          // TLS certificate; present together with keyFile enables httplib::SSLServer
    std::string keyFile;           // TLS private key; present together with certFile enables TLS
    bool requireAuth;              // when true, a missing token is a startup Fail-Fast (true recommended in production)
    int rateLimitRps;              // sensitive-endpoint token-bucket rate (per second)
    int rateLimitBurst;            // token-bucket burst cap; over limit -> 429
    std::string webRoot;           // static front-end root (Vue build output); empty = do not serve static pages
    // Note: the token is not in the config block; it is read from the environment variable MYPROT_API_TOKEN, masked in logs

    WebApiConfig()
        : bindAddress("127.0.0.1"), requireAuth(false)
        , rateLimitRps(5), rateLimitBurst(10), webRoot("webui/dist") {}
};

struct DeviceConfig {
    std::string id;
    std::string protocol;
    ConnectionConfig connection;
    int requestTimeoutMs;                  // single request-response timeout (ms); shared by all operations of this device - the sole config point (converged 2026-08-24)
    Optional<std::string> username;  // protocol-level auth credentials: injected as a template variable during handshake, masked in logs
    Optional<std::string> password;
    Optional<ResilienceConfig> resilience; // per-device resilience override; unset = use the global ConfigRoot.resilience
    // device-level template-variable defaults (§4): merged at load time into each tag's effective variable set, with the tag's explicit declaration taking precedence
    std::map<std::string, uint32_t> variables;

    DeviceConfig() : requestTimeoutMs(3000) {}
};

// ────────── protocol-family convention key names (the engine <-> protocol-template interface contract) ──────────
// All protocol-name literals in the engine converge into the following convention functions (2 cross-protocol byte units,
//   plus BitOffset); otherwise Core carries no protocol knowledge.
//   {StartByteAddress/ByteCount} are the cross-protocol byte units (engine-internal);
//   {StartAddress/RegisterCount} are protocol-family units (the protocol JSON template derives them from the former two via derivedLength;
//   Modbus: {StartByteAddress}/2 = register number; S7: direct byte address;
//   coil-class: StartByteAddress*8 + BitOffset = coil bit address).

/// start-byte-address variable name - cross-protocol byte unit
inline std::string StartByteAddressVariableName() {
    return "StartByteAddress";
}

/// data-area byte-span variable name - cross-protocol byte unit
inline std::string ByteCountVariableName() {
    return "ByteCount";
}

// StartAddressVariableName / RegisterCountVariableName do not exist -
//   the protocol-family unit names (StartAddress/RegisterCount) are declared by the protocol JSON outputs(derivedLength) itself;
//   the engine never looks them up; the validator's template-reference-scope whitelist has been changed to collect
//   derived names dynamically from protocol.outputs / op.outputs
//   (ConfigDeepValidator), no longer assuming any concrete name.

/// The variable name injected by default on the write path - not a contract name, only the default value of TagDefinition::writeVariable
///   (a tag can override it via writeVariable; templates consume {WriteValue:X4} or {WriteValue:raw} as needed).
const char* const kDefaultWriteValueVariable = "WriteValue";

/// The scope domain a bit offset plays in a derivedLength expr - not a contract name.
///   Protocol inputs must declare a same-named static default 0 (to satisfy the expr-reference-scope validation);
///   the tag-side value comes from TagDefinition::bitOffset, injected under this key by TagReader to override the protocol default.
const char* const kBitOffsetExprVariable = "BitOffset";

// ────────── framing reserved names (single source of truth) ──────────
//   These names are interpreted by the built-in engine - protocol variable names / tag variable names / variableAliases
//   must not declare them (the validator errors, see ConfigDeepValidator's reserved-name conflict check).
//   Sole source is here; forbidden to keep literal sets in ConfigDeepValidator / AutoComputeProvider /
//   FrameConsistencyCheck; now converged here, each name made a constant for direct reference at every use point.

const char* const kFramePrimitiveName = "Frame";      // {Frame:fixed} template primitive (total width of the template's fixed segments)
const char* const kExprMagicFrameLen  = "__frameLen"; // expr magic variable = bytes generated so far (excluding this segment itself)
const char* const kExprMagicFrameEnd  = "__frameEnd"; // same as __frameLen, clearer semantics
const char* const kPropLength         = "len";        // {Name:len} takes the variable's byte width
const char* const kPropOffset         = "offset";     // {Name:offset} takes the offset of the placeholder's first occurrence
const char* const kPropFixed          = "fixed";      // {Frame:fixed} property

/// All reserved names - shared by the validator's variable-name / alias conflict checks.
const char* const kFrameReservedNames[] = {
    kFramePrimitiveName, kExprMagicFrameLen, kExprMagicFrameEnd,
    kPropLength, kPropOffset, kPropFixed
};
const int kFrameReservedNameCount =
    static_cast<int>(sizeof(kFrameReservedNames) / sizeof(kFrameReservedNames[0]));

/// Whether a name is a framing reserved name.
inline bool IsFrameReservedName(const std::string& name) {
    for (int i = 0; i < kFrameReservedNameCount; ++i) {
        if (name == kFrameReservedNames[i]) return true;
    }
    return false;
}

// ────────── tag definition ──────────

/// Post-read converter (C1, ROADMAP #8 promotion item): acquired value -> engineering value, applied in array order.
/// Read path only (ResponseParser); the write path does no reverse conversion (write semantics ambiguous, see the ROADMAP note).
/// v1 supports only scale: value' = value * k + b. Typical: temperature 0.1°C/bit -> k=0.1, b=0;
///   4-20mA calibration -> k=(range upper-lower)/27648, b=lower.
struct ScaleConverter {
    double k;   // scale factor
    double b;   // offset

    ScaleConverter() : k(1.0), b(0.0) {}
};

struct TagDefinition {
    std::string name;                   // globally unique (e.g. "PLC-001.Temperature")
    std::string deviceId;
    std::string operation;              // operation name (e.g. "ReadHoldingRegisters")
        // tag variable table - cross-protocol byte units:
    //   StartByteAddress / ByteCount are engine contract names (TagGrouper takes the address / derived-length references);
    //   protocol-family units (Modbus's StartAddress/RegisterCount, S7's DB/offset family) are not written in the tag -
    //   they are derived by the protocol JSON's outputs(derivedLength). A tag explicitly declaring a same-named key overrides the derived value (validator warning).
    std::unordered_map<std::string, uint32_t> variables;
    int scanRateMs;
        // the registerCount field does not exist - the byte span is derived by the protocol outputs.ByteCount (derivedLength).
    //   The engine has zero hardcoding (no "register = 2 bytes" assumption); the cross-protocol byte span is expressed uniformly within the protocol JSON.
    //   Modbus: variables.RegisterCount (protocol-family unit) -> derivedLength.ByteCount = {RegisterCount} * 2
    //   S7:     variables.ByteCount (byte unit) directly
    std::string finalType;              // conversion target type (Config_Schema §6)
    Optional<ByteOrder> byteOrder; // unset = fallback chain: protocol dataByteOrder -> BigEndian
        // deadband / reportMode do not exist - the reporting filter has no consumption landing point (the sole result
    //   outlet onResults connects directly to LatestValueStore; filtering there would distort the "latest-value cache"); see the ROADMAP reporting-filter item.
    bool coalesce;                      // whether it participates in address-proximity coalescing; a single-address read (e.g. S7 ReadVar) sets false
        // bit offset (0-7), semantics = the bit offset within the byte pointed to by StartByteAddress; -1 = undeclared.
        //   a first-class field (no longer expressed via the variables["BitOffset"] magic key):
    //     - ResponseParser's Bool bit extraction reads this field directly (no longer consulting the variable table);
    //     - TagReader, when bitOffset >= 0, injects the expr-scope key kBitOffsetExprVariable,
    //       for protocol outputs derivedLength to reference (e.g. Modbus FC05/FC15 bit-addressing derivation).
    int bitOffset;
        // -- write tag: when direction="write", this tag defines a write rather than an acquisition point --
    // operation points directly at the write-operation template (e.g. "WriteVar"/"WriteSingleRegister"),
    // variables carry the full write semantics (TransportSize/Length/DBNumber/AddrLo/...),
    // and it does not participate in polling; /api/data/write routes to this tag by tag name.
    std::string direction;              // "read" (default, participates in polling) | "write" (write-only tag, does not participate in polling)
    std::string writeVariable;          // template variable name the value/bytes are injected into (default "WriteValue",
                                        //   templates consume {WriteValue:X4} or {WriteValue:raw} as needed)
    std::string readBackTag;            // write tag: the read-tag name referenced by the post-write read-back check
                                        //   (empty = use the write tag's own read semantics to re-read as-is;
                                        //   v1.x has replaced the "ReadHoldingRegisters" hardcoded convention)
                                        // Note: the request-response timeout is not configured here; it is decided uniformly by device.requestTimeoutMs (converged 2026-08-24)
        // -- tag-level write capability: a direction=read tag declares write semantics here --
    // Three forms: read-only (both fields empty, the write API rejects explicitly)
    //         read-write (writeOperation non-empty = scalar-writable; writeBytesOperation non-empty = variable-length-writable)
    //         write-only (direction=write, operation is the write operation, does not participate in polling).
    // Write-request variable table = defaultVariables -> variables -> writeVariables -> injected {writeVariable};
    // the read-back check uses its own operation (readBackTag needs no configuration).
        // protocol-level writeOperation/writeBytesOperation fallback does not exist - the sole write-declaration point: the tag.
    std::string writeOperation;         // operation-template name used by a scalar write (POST value) (e.g. "WriteVar")
    std::string writeBytesOperation;    // operation-template name used by a variable-length write (POST bytes) (template consumes via {Name:raw})
    std::unordered_map<std::string, uint32_t> writeVariables; // write-request-specific variable overrides (e.g. S7 write's TransportSize/Length differ from read)

    // post-read conversion chain (optional; empty = no conversion, behavior identical to the old version).
    //   Takes effect only for numeric (integer/float) results; a Bool/ByteArray/String tag declaring converters is a validation error.
    //   The converted value is uniformly a Double (TypedValue::d) - readable by reporting/snapshot/write-back comparison consumers.
    std::vector<ScaleConverter> converters;

    TagDefinition()
        : scanRateMs(1000), finalType("UInt16")
        , coalesce(true)
        , direction("read"), writeVariable(kDefaultWriteValueVariable)
        , bitOffset(-1) {
        // converters have no built-in instances - a default empty chain = no conversion
    }
};

// ────────── config root (maps to tags.json) ──────────

struct ConfigRoot {
    int schemaVersion;                          // config generation number (ADR-0005); must equal kSupportedSchemaVersion
    Optional<ResilienceConfig> resilience; // global resilience policy (Config_Schema §11 / ADR-0004); unset = all defaults
    Optional<WebApiConfig> webApi;         // management-plane security (Config_Schema §12 / ADR-0008); unset = all defaults
    std::vector<DeviceConfig> devices;
    std::vector<TagDefinition> tags;

    ConfigRoot() : schemaVersion(kSupportedSchemaVersion) {}
};

}} // namespace MyProt::Core

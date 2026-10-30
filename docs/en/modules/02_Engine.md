# MyProtV2 - Engine Module

> **Series**: MyProtV2 module design
> **Previous**: [Core module](./01_Core.md)
> **Next**: [Transport module](./03_Transport.md)

---

**Depends on**: `Core` only

> WARNING: configuration contracts (template grammar, field semantics) are governed by [Config_Schema.md](../Config_Schema.md).
>
> **v4 note**: this module is a **pure concrete-class utility library** (no orchestrator, no interface layer). The early `ProtocolEngine` orchestrator, the `IExpressionEvaluator` / `IRequestBuilder` / `IResponseParser` interfaces and their factories have been removed - pipeline assembly now lives in the Gateway layer's `TagReader` (see [05_Gateway.md](./05_Gateway.md)); Engine only provides independently callable build/parse/evaluate/convert primitives.

## Module composition

| Header | Contents |
|--------|------|
| `MiniExpression.hpp` | Unified expression engine (`validCondition` / `derivedLength` / handshake conditions / session extraction), the same parser at load time and at run time |
| `RequestBuilder.hpp` | Request template expansion, template layout and derived-length injection |
| `AutoComputeProvider.hpp` | `source=auto` variable evaluation (autoIncrement / frameSlice / expr / crc) |
| `ResponseParser.hpp` | Response validation, data extraction, byte-order resolution |

## 2.1 Expression grammar (EBNF)

```
expression    := logical_or

logical_or    := logical_and ( "||" logical_and )*
logical_and   := bitwise_or ( "&&" bitwise_or )*
bitwise_or    := bitwise_xor ( "|" bitwise_xor )*
bitwise_xor   := bitwise_and ( "^" bitwise_and )*
bitwise_and   := equality ( "&" equality )*
equality      := relational ( ("==" | "!=") relational )*
relational    := shift ( ("<" | ">" | "<=" | ">=") shift )*
shift         := additive ( ("<<" | ">>") additive )*
additive      := multiplicative ( ("+" | "-") multiplicative )*
multiplicative:= unary ( ("*" | "/" | "%") unary )*
unary         := ("!" | "-" | "~") unary | primary
primary       := NUMBER | HEX_NUM | "resp" "[" index_expr "]" | "(" expression ")"
index_expr    := NUMBER | NUMBER ":" NUMBER    # resp[5:9] = bytes 5..9 (inclusive)
NUMBER        := [0-9]+
HEX_NUM       := "0x" [0-9a-fA-F]+
```

> **Precedence is aligned with C (KI-13)**: from loosest to tightest, `||` < `&&` < `|` < `^` < `&` < `== !=` < `< > <= >=` < `<< >>` < `+ -` < unary `! - ~`.
> Note: `==` binds tighter than `|`/`&` (as in C), so "combine bits first, then compare" **requires explicit parentheses**, e.g. `((resp[0]<<8)|resp[1])==0x0016`; writing `(resp[0]<<8)|resp[1]==0x0016` parses as `(resp[0]<<8)|(resp[1]==0x0016)`, which is wrong.
>
> **v1 scope**: only the `resp` byte-stream scope is supported; session variables cannot be referenced.

Supported examples:

```
"resp[1]==0x03"                    // single-byte check
"resp[5]==0xD0"                    // S7 COTP handshake check
"((resp[0]<<8)|resp[1])==0x0016"   // two-byte combination (parentheses needed because == binds tighter than | &)
"resp[2]>=4"                       // length check
"resp[4]*256+resp[5]"              // assemble a length from two bytes
```

## 2.2 MiniExpression - unified expression engine

> **Unified in 2026-10**: the former `ExpressionEvaluator` (handshake conditions / session
> extraction) has been removed. Handshake conditions, the integer fallback of
> `sessionExtractExpr`, `validCondition` and `derivedLength` now share the same
> recursive-descent parser and the same precedence table (§2.1).

```cpp
// src/Engine/include/MyProt/Engine/MiniExpression.hpp

namespace MyProt { namespace Engine { namespace MiniExpression {

class Error : public std::runtime_error { /* ... */ };

/// Variable lookup callback: found=false when missing
using VarLookup = std::function<uint64_t(const std::string&, bool& found)>;

class Parser {
public:
    explicit Parser(const std::string& expr);
    void Parse();                          // throws Error on syntax errors
    uint64_t Evaluate(const VarLookup& lookup) const;
    bool IsParsed() const;
};

}}} // namespace MyProt::Engine::MiniExpression
```

Implementation notes:

- The precedence chain maps one-to-one onto the §2.1 EBNF:
  `Or -> And -> BitOr -> BitXor -> BitAnd -> Equality -> Relational -> Shift -> Expr -> Term -> Unary -> Primary`;
- `resp[N]` calls `VarLookup` with the canonical name `"resp[<index>]"` (the index may itself be an
  expression); the caller performs the bounds check - `ChannelManager` treats out-of-range as a
  missing variable;
- `resp[A:B]` slices never enter generic expressions; the handshake/session-extraction path handles them separately;
- `{name:len}` is parsed by `ParseLenToken`, with the byte-width table supplied by `AutoComputeProvider`;
- Parse/evaluate failures (unknown variable, division by zero, illegal token) throw `Error`, which
  callers translate into `Expected` failures or an `InvalidResponse` quality code.

## 2.3 RequestBuilder - template expansion

```cpp
// src/Engine/include/MyProt/Engine/RequestBuilder.hpp

namespace MyProt { namespace Engine {

/// Stateless template expander (called directly by E2E/Gateway)
///
/// Template fragment grammar:
///   "0A1B"             -> hex literal (every 2 chars = 1 byte)
///   "{Name:X4}"        -> variable as 4 hex digits (= 2 bytes), big-endian
///   "{Name:raw}"       -> raw variable-length byte stream (write payload)
///   Note: auto evaluation is declared by inputs.source=auto,
///         the template does not accept an `:auto:` token
class RequestBuilder {
public:
    /// Expand the operation template into the request byte stream (scalar variable path)
    /// @param op operation config (requestTemplate is a sequence of fragments)
    /// @param variables tag variable set ({ "StartAddress": 0, ... })
    /// @param varAliasMap variable alias map (alias -> internal name)
    /// @param autoProvider declarative auto evaluator
    /// @return the request byte stream, or BuildError on failure
    Core::Expected<Core::Bytes> Build(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);

    /// Variable-length payload path: {Name:raw} takes bytes from variableBytesHex
    Core::Expected<Core::Bytes> BuildBytes(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& variableBytesHex,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);
};

}} // namespace MyProt { namespace Engine
```

> **v1 boundary**: the builder is single-pass and supports only the three fragment kinds above. `calc` placeholders such as `{L:calc:Xn}` are **not implemented** - their presence in a template reports `BuildError` (validator rule 7 "uses undeclared function"; E2E asserts it). Length fields are supplied directly by tag variables (Modbus TCP, for instance, uses `RegisterCount` together with the batch-merge logic).

## 2.4 AutoComputeProvider - declarative auto evaluation

```cpp
// src/Engine/include/MyProt/Engine/AutoComputeProvider.hpp

namespace MyProt { namespace Engine {

/// Driven by the protocol JSON inputs.source=auto declaration
/// Strategies: autoIncrement / frameSlice / expr / crc
class AutoComputeProvider {
public:
    bool DeclareJson(const std::string& autoComputeJson);
    uint64_t Resolve(const std::string& name, int byteWidth, const BuildContext& ctx);
    void Reset(const std::string& name);
};

}} // namespace MyProt { namespace Engine
```

Callers (Gateway single read / batch read / write path) share one instance, so the transaction id of a device increases uniquely across the whole chain.
`derivedLength` does not go through this class: the parameter layer pre-computes it via `RequestBuilder::InjectDerivedLengthVariables` and injects it into the variable pool.

## 2.5 ResponseParser - response parsing

```cpp
// src/Engine/include/MyProt/Engine/ResponseParser.hpp

namespace MyProt { namespace Engine {

/// Parse flow:
///   1. validCondition check (MiniExpression driven, supports resp[N] plus comparison/logic)
///   2. locate the data area at dataStartIndex
///   3. registerCount*2 bytes (or everything remaining if short) converted by finalType
///   4. produce a TagValue (quality=Good, timestamp=now)
class ResponseParser {
public:
    /// Byte-order resolution: tag override > protocol dataByteOrder > default big-endian
    /// (the single decision point for every pipeline - single read, batch read, ... -
    ///  so the three-step fallback is not duplicated)
    static Core::ByteOrder ResolveByteOrder(
        const Core::ProtocolConfig& protocol,
        const Core::Optional<Core::ByteOrder>& tagOverride);

    /// Validate the validCondition subset "resp[N]==V"
    /// (write-ack echo and similar cases only need a condition check, no data parsing)
    static bool CheckCondition(const Core::ByteView& response,
                               const std::string& validCondition);

    /// Parse a response and convert it into a tag value
    /// @return the tag value, or InvalidResponse/ParseError/TypeConversionError
    Core::Expected<Core::TagValue> Parse(
        const Core::ByteView& response,
        const Core::ResponseParserConfig& config,
        const Core::TagDefinition& tag,
        Core::ByteOrder byteOrder);
};

}} // namespace MyProt { namespace Engine
```

`Parse` calls the byte-order assembly helpers `Core::FromBytesU16/U32/U64` directly by finalType: integers are interpreted in the requested byte order, floats are assembled word-wise and reinterpreted by IEEE 754 bit copy (C++11 `memcpy`), `Bool` takes the first byte as non-zero, `String` copies the raw bytes, and quality codes are mapped per ADR-0006; `rawData` keeps the raw bytes on `Uncertain` for diagnostics.

---

> Un-implemented extensions are tracked in [ROADMAP.md](../../ROADMAP.md).
> **Previous**: [Core module](./01_Core.md)
> **Next**: [Transport module](./03_Transport.md)

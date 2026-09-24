// src/Engine/include/MyProt/Engine/AutoComputeProvider.hpp
// General auto-compute evaluator (driven by the protocol JSON top-level inputs.source=auto declarations)
//  - Declaration: the protocol JSON inputs.source=auto section declares {strategy: autoIncrement|frameSlice|expr|crc}
//  - Invocation: RequestBuilder.RenderTemplate calls Resolve(name, byteWidth, ctx) when it meets an auto placeholder
//         Inside Resolve: route to a strategy per the declaration; undeclared falls back to atomic auto-increment (defensive, not reached in normal flow)
//
// 4 built-in strategies:
//   autoIncrement : atomic +1 increment (seedable)               — modbus TransactionID
//   frameSlice    : whole-packet / segment-length / byte slice    — TPKT_Length/MBAP_Length
//   expr          : arithmetic expression (numbers/vars/+-*/%&|^~parens) — const + arbitrary arithmetic
//   crc           : polynomial checksum (crc16-modbus / crc32)    — modbus-RTU CRC
//
// BuildContext (passed by value, zero dependency):
//   variables          : current variable pool (includes tag/protocol default/autoCompute injections)
//   frameSoFar         : the byte stream currently being built (frameSlice needs to read it)
//
// The header does not include nlohmann/json.hpp (avoiding Engine's dependency on nlohmann);
// Declare takes a std::string (the autoCompute section as a JSON string), parsed internally in the cpp.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

namespace MyProt { namespace Engine {

// Minimal JSON value type - fully defined only in the cpp; forward-declared here so the header can use const JVal&
struct JVal;

/// Context of one template build (request/response framework byte stream + variable pool)
struct BuildContext {
    /// Current variable pool (scalars), { "StartAddress": 100, "RegisterCount": 4, ... }
    const std::unordered_map<std::string, uint32_t>* variables = nullptr;
    /// Byte stream already produced by the current template expansion (read by frameSlice)
    /// Note: at each :auto evaluation this stream is the "generated prefix" (when TPKT_Length computes the whole packet, = the entire prefix)
    const std::vector<uint8_t>* frameSoFar = nullptr;
};

/// Template layout (ADR-0012 §1.2) - derived from a single scan of requestTemplate,
/// shared by {Frame:fixed} / {Name:offset} derived-length evaluation and save-time trial validation.
///   widths     : placeholder name -> render width ({N:raw} = kRawMarker; unknown format recorded as 0 + hasUnknown)
///   offsets    : placeholder name -> cumulative byte offset before its first occurrence (raw-payload placeholders also recorded)
///   fixedTotal : sum of all non-raw element widths (the frame's fixed segment, incl. each Xn placeholder's own width)
///   hasUnknown : an unknown-format placeholder occurred (the validator should report an error; layout counted as 0)
struct TemplateLayout {
    static const uint32_t kRawMarker = 0xFFFFFFFFu;
    std::unordered_map<std::string, uint32_t> widths;
    std::unordered_map<std::string, uint32_t> offsets;
    uint32_t fixedTotal;
    bool hasUnknown;
    TemplateLayout() : fixedTotal(0), hasUnknown(false) {}
};

/// Auto-compute evaluator - driven by the protocol inputs.source=auto declarations
///
/// Thread safety: the three mutable states counters / rules / exprCache are each guarded by an independent mutex;
///   strategy execution and expression evaluation both happen outside the lock (rules published as shared_ptr<const>, readers take a snapshot).
///   A repeated DeclareJson with identical content takes the fast path (skips JSON parsing), fitting the "sync rules per request" call model.
class AutoComputeProvider {
public:
    AutoComputeProvider();
    ~AutoComputeProvider();

    /// Reset a variable's auto-increment counter (test/ops helper)
    void Reset(const std::string& name);

    /// Load the auto declaration section (as a string)
    /// e.g. "{\"TransactionID\":{\"strategy\":\"autoIncrement\",\"params\":{\"seed\":1}}}"
    /// Empty/invalid: silently ignored (no auto declarations)
    /// Parse failure: returns false (the protocol JSON as a whole remains usable; only the auto section is invalidated)
    /// Content identical to the previous declaration: return true directly (skip parsing and rule rebuild)
    bool DeclareJson(const std::string& autoComputeJson);

    /// Called when a :auto is met in the template: prefers the declared strategy, falls back to Next() increment when undeclared
    uint64_t Resolve(const std::string& name, int byteWidth, const BuildContext& ctx);

    /// Check: whether a variable is declared by the autoCompute section (for debugging/validation)
    bool IsDeclared(const std::string& name) const;

    /// Determine whether a declared variable is autoIncrement (for parameter-layer pre-parsing).
    ///   Parameter layering: autoIncrement/derivedLength belong to the "parameter layer" (pre-parsed into the parameter table before rendering);
    ///   frameSlice/expr(references __frameLen)/crc belong to "frame-aware" (evaluated per-frame at render time, cannot be lowered).
    bool IsAutoIncrement(const std::string& name) const;

    /// strategy=derivedLength derived-length evaluation (consumed at the parameter-layer pre-parsing stage)
    ///   Single expression form: uses expr (e.g. "{Payload:len} + 7" / "{Payload:len} * 8"); the kind field is not part of this layer's contract.
    ///   inputs is the merged flat input-parameter pool (may be a null pointer) — expr may reference any ready input name within it
    ///         (the validator has guaranteed the reference domain).
    ///   {name:len} references the byte length of an inputs variable — a payload variable = actual byte count (varLen injected by the caller),
    ///         others = template render width; a plain-name reference looks up the inputs value pool. No payload/count reserved names.
    ///   Template-structure primitives (ADR-0012 §1.1) — {Frame:fixed} (sum of the template's non-raw element widths,
    ///         looks up layout->fixedTotal) and {name:offset} (cumulative offset before a placeholder's first occurrence, looks up layout->offsets);
    ///         when layout is nullptr both primitives are unresolvable (evaluation fails), {name:len} and plain-name references are unaffected.
    ///   On success returns true and writes out; on expression-evaluation failure returns false (fills errMsg with the reason when non-null).
    static bool ResolveDerivedLength(
        const std::string& expr,
        const std::unordered_map<std::string, uint32_t>* inputs,
        const std::unordered_map<std::string, uint32_t>* varLen,
        const TemplateLayout* layout,
        uint32_t& out,
        std::string* errMsg = nullptr);

private:
    // PIMPL: the internal implementation holds (counters, rules, exprCache); definition in the .cpp
    struct Impl;
    std::unique_ptr<struct Impl> _impl;

    /// Get the next value of an atomic auto-increment counter (mod 2^(8*byteWidth)) - internal helper,
    ///   used by ExecAutoIncrement and Resolve's fallback path.
    uint64_t Next(const std::string& name, int byteWidth);

    // Internal: the 4 strategy implementations (the minimal JSON value type JVal is defined on the cpp side, within this namespace as MyProt::Engine::JVal)
    uint64_t ExecAutoIncrement(const std::string& name, int byteWidth, const JVal& params);
    uint64_t ExecFrameSlice   (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
    uint64_t ExecExpr         (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
    uint64_t ExecCrc          (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
};

}} // namespace MyProt::Engine

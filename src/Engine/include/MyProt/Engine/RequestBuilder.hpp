// src/Engine/include/MyProt/Engine/RequestBuilder.hpp
// Request builder - template-based request generation (C++11, ADR-0010 §5)

#pragma once
#include <string>
#include <unordered_map>
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"

namespace MyProt { namespace Engine {

// ────────── concrete builder - stateless template expansion (used directly by E2E/Gateway) ──────────

/// Template-fragment syntax definition:
///   "0A1B"             -> hex literal (every 2 chars = 1 byte)
///   "{Name:X4}"        -> variable output as 4 hex digits (= 2 bytes), big-endian
///   "{Name:Xn}"        -> whether it is auto is decided by the autoCompute declaration (no :auto: token accepted)
///   "{Name:raw}"       -> variable-length byte injection (looks up the variableBytesHex table; contract: ADR-0007 §3)
class RequestBuilder {
public:
    /// Expand the operation template to generate a request frame (scalar-variable path)
    /// @param op operation config (requestTemplate is a sequence of template fragments)
    /// @param variables tag variable table ({ "StartAddress": 0, ... })
    /// @param varAliasMap variable alias map (alias -> internal name); an empty map = no aliases
    /// @param autoProvider declarative auto evaluator (for variables declared in the autoCompute section)
    /// @return the request byte stream; returns BuildError on failure
    Core::Expected<Core::Bytes> Build(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);

    /// Expand the operation template to generate a request frame (variable-length-variable path, P1 A)
    /// Scalar variables look up variables; {Name:raw} looks up variableBytesHex (hex string -> byte stream);
    /// the two tables can be used independently or together (different placeholders within the same op route to different tables)
    Core::Expected<Core::Bytes> BuildBytes(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& variableBytesHex,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);

    // Merge protocol-level static variables + tag-level variables
    // Returns a new merged map: protocol-level static variables as the base, tag.variables overrides same-named keys
    // The caller passes the return value to Build/BuildBytes; the original map is not modified
    static std::unordered_map<std::string, uint32_t> MergeVariables(
        const std::unordered_map<std::string, uint32_t>& protocolDefaults,
        const std::unordered_map<std::string, uint32_t>& tagVariables);

    // Three-way merge (protocol-level defaultVariables + op-level static overrides + tag-level overrides).
    //   Only entries in op.inputs with source=static and a value enter ctx.variables (auto goes through AutoComputeProvider;
    //   valueless static entries do not enter ctx). Precedence: tag > op > protocol.
    static std::unordered_map<std::string, uint32_t> MergeVariables(
        const std::unordered_map<std::string, uint32_t>& protocolDefaults,
        const std::unordered_map<std::string, uint32_t>& opStaticVariables,
        const std::unordered_map<std::string, uint32_t>& tagVariables);

    // Rebuild variable semantics from the protocol inputs/outputs sections (config shape: input static in inputs, derived outputs in outputs;
    //   there is no flat single variables section - generation changes: see ADR-0005).
    //   CollectStaticVariables  -> inputs.source=static entries -> protocol-level base of the ctx.variables merge chain.
    //   CollectAutoComputeJson  -> inputs.source=auto and non-derivedLength entries ->
    //     AutoComputeProvider::DeclareJson format; outputs(derivedLength) are excluded (special-cased at the WriteBytes stage).
    static std::unordered_map<std::string, uint32_t> CollectStaticVariables(
        const Core::ProtocolConfig& protocol);
    static std::string CollectAutoComputeJson(const Core::ProtocolConfig& protocol);

    /// Splice op-level auto (non-derivedLength) inputs into the protocol-level autoComputeJson.
    /// Single implementation - shared by Gateway (TagReader) and Service (FrameConsistencyCheck),
    /// avoiding semantic drift between the two sides; derivedLength does not enter this section (injected by the parameter layer's InjectDerivedLengthVariables).
    static std::string MergeOpAutoComputeJson(const Core::ProtocolConfig& protocol,
                                              const Core::OperationConfig& op);

    // ── template layout & derived-length injection (ADR-0012); the sole implementation is in Engine, shared by Gateway and Service ──

    /// Scan requestTemplate to produce the layout table (widths / first-occurrence offsets / total fixed-segment width).
    /// Consistent with RenderTemplate's element bisection: whole-element placeholder ({...}) or hex literal;
    /// an unknown format is recorded as hasUnknown (width counted as 0). Shared by Gateway and Service trial-validation, avoiding dual-implementation drift.
    static TemplateLayout BuildTemplateLayout(
        const std::vector<std::string>& requestTemplate);

    /// Derived-length variable injection (parameter-layer pre-parsing): scan protocolOutputs ∪ opOutputs for
    /// source=auto strategy=derivedLength declarations, evaluate by expr and inject into variables
    /// (a tag-provided same-named variable takes precedence - the caller must place explicit values first).
    /// totalBytes = the actual total byte count of template {N:raw} payloads; layout = the BuildTemplateLayout product.
    /// When expr evaluation fails, that variable is not injected (rendering will BuildError "template variable not provided").
    /// Returns void: Engine carries no protocol-family contract name (e.g. PDULength) - the caller does not need
    ///   a "does it contain a certain protocol field" conclusion either, so do not reintroduce protocol conventions into this layer via a return value.
    static void InjectDerivedLengthVariables(
        std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, Core::VariableConfig>& protocolOutputs,
        const std::unordered_map<std::string, Core::VariableConfig>& opOutputs,
        std::size_t totalBytes,
        const TemplateLayout& layout);
};

}} // namespace MyProt::Engine

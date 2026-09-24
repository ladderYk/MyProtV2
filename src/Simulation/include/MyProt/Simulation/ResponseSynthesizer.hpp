// src/Simulation/include/MyProt/Simulation/ResponseSynthesizer.hpp
// Response synthesizer - reverse reuse of responseParser (config-driven simulation L3 layer)
//
// The client validates responses with responseParser; the simulator treats the same spec as a "response layout manual"
// and generates a response frame in reverse from the request (Config_Schema §5):
//   validCondition "resp[N]==V" -> write literal V at response offset N (e.g. the FC echo bit)
//   dataLengthExpr  "resp[N]"   -> write the actual data length at response offset N
//   dataStartIndex M            -> mirror the request's first M bytes as the response prefix, the data region starts at M
//   dataLengthExpr a numeric constant -> align the data-region length to that constant (truncate/zero-pad)
// Finally recompute the length field per framing - inverse to LengthFieldFrameParser::CalculateTotalFrameSize's
// frame-splitting formula, guaranteeing the client can correctly split out this frame with the same framing config.

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstddef>

#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Simulation {

class ResponseSynthesizer {
public:
    /// The response-layout spec parsed from OperationConfig
    struct Spec {
        std::vector<std::pair<std::size_t, std::uint8_t> > asserts; ///< offset -> literal
        bool hasLenIndex;   ///< dataLengthExpr shaped like resp[N]
        std::size_t lenIndex;
        std::size_t constLen;     ///< dataLengthExpr is a numeric constant (0 = none)
        std::size_t dataStartIndex;

        Spec() : hasLenIndex(false), lenIndex(0), constLen(0), dataStartIndex(0) {}
    };

    /// Parse an operation's response spec. Grammar clauses that are unsupported are silently ignored (lenient parsing).
    static void ParseSpec(const Core::OperationConfig& op, Spec& out);

    /// Synthesize a response frame.
    /// @param request the matched complete request frame (source of the prefix mirror)
    /// @param data    the response data-region content (the upper layer takes it out of the data region per variable)
    /// @param framing the protocol framing config (basis for recomputing the length field)
    /// @return the response frame; returns empty when it cannot be synthesized
    static std::vector<std::uint8_t> Synthesize(const Spec& spec,
                                                const std::vector<std::uint8_t>& request,
                                                const std::vector<std::uint8_t>& data,
                                                const Core::FramingConfig& framing);

    /// Synthesize a frame from a custom response template (SimOperationConfig.responseTemplate; when non-empty
    /// it overrides the echo reverse synthesis). Grammar:
    ///   hex literal  "AA 0B"      - per-byte constants (space/tab separated)
    ///   {req:N:M}               - copy M bytes from request-frame offset N (echo)
    ///   {data}                  - expand the response data region (read = register values; write is empty)
    /// After concatenation, recompute the length field per framing (consistent with Synthesize).
    /// @return true on success; false on a grammar error or request out-of-range (the caller should not respond)
    static bool SynthesizeFromTemplate(
        const std::vector<std::string>& tmpl,
        const std::vector<std::uint8_t>& request,
        const std::vector<std::uint8_t>& data,
        const Core::FramingConfig& framing,
        std::vector<std::uint8_t>& out);

private:
    /// Recompute and write the in-frame length field per LengthFieldConfig
    static void ApplyLengthField(std::vector<std::uint8_t>& frame,
                                 const Core::LengthFieldConfig& lf);
};

}} // namespace MyProt::Simulation

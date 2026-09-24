// src/Simulation/include/MyProt/Simulation/TemplateMatcher.hpp
// Request-template matcher - reverse reuse of requestTemplate (config-driven simulation L2 layer)
//
// The client builds requests with requestTemplate; the simulator compiles the same template into a "byte-shape pattern",
// recognizing a received complete request frame in reverse: yielding the operation name + extracting variable values at placeholder positions.
//
// The grammar matches RequestBuilder (Config_Schema §3.2, the actually-implemented subset):
//   - hex-literal line: an even-length hex string, every 2 chars = 1 byte (spaces ignored)
//   - placeholder {Name:Xn}      -> fixed-width variable segment, extracts a big-endian value on match
//   - placeholder {Name:auto:Xn} -> fixed-width wildcard segment (auto-increment fields like TransactionId), content skipped
//   - placeholder {Name:raw}     -> variable-length tail segment (only allowed as the template's last line): on match it
//     consumes all remaining bytes of the frame, capturing nothing; data extraction for write scenarios is
//     handled by server.json dataOffset. Variable-length writes (e.g. Modbus FC16 {WriteValue:raw}) can thus be recognized in reverse by the simulator.
//   Xn: hex-digit width, even 2..16 -> n/2 bytes.
//   Other formats (checksum-function placeholders etc.) are skipped at compile time - kept consistent with the grammar the builder does not implement.

#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstddef>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Simulation {

/// Match result
struct TemplateMatch {
    bool matched;
    std::string operation;                      ///< the matched operation name
    std::map<std::string, std::uint32_t> variables; ///< variables extracted at placeholder positions
    std::size_t frameLength;                    ///< expected frame length (bytes)

    TemplateMatch() : matched(false), frameLength(0) {}
};

class TemplateMatcher {
public:
    /// protocol must remain valid for the lifetime (a reference to it is held)
    explicit TemplateMatcher(const Core::ProtocolConfig& protocol);

    /// Compile all operation templates; must be called again after the protocol content changes
    void Compile();

    /// Match against a complete request frame; when multiple operations hit, return the first in protocol declaration order.
    /// frame must be a framing-split complete request (TCP split by FrameParser).
    TemplateMatch Match(const Core::ByteView& frame) const;

    /// Compile-time ambiguity report (the result of the last Compile()):
    /// operation pairs with the same total length and byte-wise-compatible constraints - a frame can hit both,
    /// Match returns only the first in compile order, so actual routing may defy config intent (warning only, does not block)
    const std::vector<std::string>& Ambiguities() const { return _ambiguities; }

private:
    struct Segment {
        enum Kind { Literal, Variable, Wildcard, Raw } kind;
        std::vector<std::uint8_t> literal; ///< Literal: expected byte sequence
        std::string varName;               ///< Variable: variable name; Raw: tail-segment variable name (not captured)
        int widthBytes;                    ///< Variable/Wildcard: fixed-width byte count; Raw always 0
        Segment() : kind(Literal), widthBytes(0) {}
    };
    struct CompiledOp {
        std::string name;
        std::vector<Segment> segments;
        std::size_t totalSize; ///< fixed-length pattern: total pattern byte length (frame length must match exactly);
                               ///< variable-length-tail pattern (hasTail): the minimum byte length of the fixed prefix
        bool hasTail;          ///< the last segment is a {Name:raw} variable-length tail
        CompiledOp() : totalSize(0), hasTail(false) {}
    };

    static bool CompileLine(const std::string& raw, std::vector<Segment>& segs,
                            std::size_t& lineSize);
    static std::size_t WidthSpecToBytes(const std::string& widthSpec);

    const Core::ProtocolConfig& _protocol;
    std::vector<CompiledOp> _ops;
    std::vector<std::string> _ambiguities;
};

}} // namespace MyProt::Simulation

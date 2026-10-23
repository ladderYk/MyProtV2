// src/Core/include/MyProt/Core/TemplateGrammar.hpp
// Request-template grammar - the single executable definition of the template syntax.
//
// Before this header existed the grammar was implemented three times:
//   Engine/RequestBuilder (render), Simulation/TemplateMatcher (reverse match),
//   Service/ConfigDeepValidator (load-time validation).
// All three now parse through this module; only the use of the parse result differs.
//
// Grammar (Config_Schema §3.2):
//   literal element : whitespace-separated hex pairs ("00 06" / "0006"), every 2 chars = 1 byte
//   {Name:Xn}       : fixed-width variable, n = even hex-digit count 2..16 -> n/2 bytes, big-endian
//   {Name:XnLE}     : same width, little-endian wire order
//   {Name:raw}      : variable-length byte injection (only BuildBytes / write payloads)
//   {Name:auto:Xn}  : legacy wildcard form, accepted by the simulator only (not by render/validation)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MyProt { namespace Core { namespace TemplateGrammar {

/// Parsed template element kind.
enum SegmentKind {
    kLiteral,   ///< hex literal byte sequence
    kVariable,  ///< fixed-width variable placeholder
    kWildcard,  ///< fixed-width placeholder whose value is ignored (legacy {Name:auto:Xn})
    kRaw        ///< variable-length byte placeholder ({Name:raw})
};

/// One parsed template element.
struct Segment {
    SegmentKind kind;
    std::string name;                  ///< kVariable / kRaw / kWildcard: variable name
    std::vector<std::uint8_t> literal; ///< kLiteral: expected bytes
    int widthBytes;                    ///< kVariable / kWildcard: byte width (kRaw = 0)
    bool littleEndian;                 ///< kVariable: XnLE suffix present

    Segment() : kind(kLiteral), widthBytes(0), littleEndian(false) {}
};

/// Result of parsing one element.
enum ParseStatus {
    ParseOk,          ///< parsed as literal or placeholder
    ParseLegacyAuto,  ///< {Name:auto:Xn}: caller decides whether to accept it
    ParseInvalid      ///< syntax error (see error out-param)
};

/// Xn / XnLE -> byte width; returns 0 when the format spec is invalid.
int WidthSpecBytes(const std::string& formatSpec);

/// Split placeholder inner text by ':'; empty segments are preserved.
std::vector<std::string> SplitSegments(const std::string& inner);

/// Parse one template element (hex literal or {...} placeholder).
/// @param strictIdentifier require [A-Za-z_][A-Za-z0-9_]* variable names (validator semantics)
/// @param allowLegacyAuto accept {Name:auto:Xn} as kWildcard (simulator reverse matching only)
/// @param error optional human-readable reason when ParseInvalid is returned
ParseStatus ParseElement(const std::string& token,
                         Segment& out,
                         bool strictIdentifier,
                         bool allowLegacyAuto,
                         std::string* error = 0);

/// Parse one template line: trims surrounding whitespace and returns the parsed segment(s).
/// A blank line is valid and contributes no segment. lineSize receives the byte length.
bool ParseLine(const std::string& raw,
               std::vector<Segment>& segments,
               std::size_t& lineSize,
               bool allowLegacyAuto,
               std::string* error = 0);

/// Parse a hex literal ("01 0A" / "010A"); whitespace is ignored.
bool ParseHexLiteral(const std::string& text, std::vector<std::uint8_t>& out);

/// Strict placeholder split used by load-time validation:
///   {Name} / {Name:fmt} / {Name:fmt:fmt}; name must be an identifier.
bool SplitPlaceholder(const std::string& token, std::vector<std::string>& parts);

/// Whether token is a valid supported placeholder: {Name:Xn} / {Name:XnLE} / {Name:raw}.
bool IsValidPlaceholder(const std::string& token);

/// Trim leading/trailing ASCII whitespace.
std::string Trim(const std::string& text);

}}} // namespace MyProt::Core::TemplateGrammar

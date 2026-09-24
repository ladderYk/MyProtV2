// src/Engine/include/MyProt/Engine/ResponseParser.hpp
// Response parser - data extraction and quality assessment (C++11, ADR-0010 §5)

#pragma once
#include <string>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11-compatible, replaces std::optional, rolled back 2026-08-29)
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Engine {

// ────────── concrete parser - single-tag response parsing (used directly by E2E/Gateway) ──────────

/// Parse flow:
///   1. validCondition check (supports "resp[N]==V", V in decimal or 0x hexadecimal)
///   2. dataStartIndex locates the data region
///   3. registerCount×2 bytes (if insufficient, take all remaining) converted per finalType
///   4. produce a TagValue (quality=Good, timestamp=now)
class ResponseParser {
public:
    /// Byte-order arbitration: tag-level override -> protocol dataByteOrder -> default big-endian
    /// (the sole arbitration point for all pipelines - single read / batch read etc., avoiding duplicated three-step fallback logic everywhere)
    static Core::ByteOrder ResolveByteOrder(
        const Core::ProtocolConfig& protocol,
        const Core::Optional<Core::ByteOrder>& tagOverride);

    /// Validate the validCondition subset "resp[N]==V"
    /// (scenarios like write-response echo that only need a condition check, without parsing the data region)
    static bool CheckCondition(const Core::ByteView& response,
                               const std::string& validCondition);

    /// Parse the response and convert it to a tag value
    /// @param response the response byte stream
    /// @param config parser config (validCondition/dataStartIndex etc.)
    /// @param tag tag definition (finalType/registerCount/name/deviceId)
    /// @param byteOrder byte order
    /// @return the tag value; returns InvalidResponse/ParseError/TypeConversionError on failure
    Core::Expected<Core::TagValue> Parse(
        const Core::ByteView& response,
        const Core::ResponseParserConfig& config,
        const Core::TagDefinition& tag,
        Core::ByteOrder byteOrder);
};

}} // namespace MyProt::Engine

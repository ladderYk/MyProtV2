// src/Service/include/MyProt/Service/ResponsePreview.hpp
// Response parse preview — "response bytes + operation + tag-like knobs -> parsed value".
//
// Why: the trial render preview answers "what will we send"; this answers "what will we
// make of what comes back". Protocol onboarding still requires a live device plus logs to
// see whether validCondition / dataStartIndex / finalType / bitOffset are right.
//
// Boundaries:
//   - read-only: no disk write, no reload, no device I/O;
//   - parses through Engine::ResponseParser (the gateway's only response path), so the
//     preview value equals the runtime value for the same tag-like inputs;
//   - a preview has no real tag, so the caller supplies the fields ResponseParser consumes
//     (finalType / bitOffset / ByteCount / byteOrder); tag-level converters are applied only
//     when the caller passes the chain explicitly (scale k/b, v5 C1).

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Service {

/// Tag-like overrides. Defaults mirror a bare tag: UInt16, no bit extraction,
/// ByteCount unset (= parse the whole data region), protocol byte order.
struct ResponsePreviewRequest {
    std::string protocolJson;   ///< candidate protocol text (comments / BOM tolerated)
    std::string operation;      ///< operation map key or op.name
    std::string hex;            ///< response bytes: "03 04 00 2A ..." or "0304002A..."
    std::string finalType;      ///< UInt16/Int16/UInt32/Int32/UInt64/Int64/Float/Double/Bool/String/ByteArray
    int bitOffset;              ///< Bool bit extraction (0..7); -1 = whole-register truthiness
    long byteCount;             ///< data-region bytes; 0 = everything after dataStartIndex
    std::string byteOrder;      ///< optional: BigEndian | LittleEndian | WordBigByteLittle | WordLittleByteBig
    std::vector<Core::ScaleConverter> converters;  ///< optional tag-level chain (v5 C1: value' = value*k + b)

    ResponsePreviewRequest() : bitOffset(-1), byteCount(0) {}
};

struct ResponsePreviewResult {
    bool ok;
    std::string protocolName;
    std::string operation;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    std::vector<std::string> operations;    ///< all operation names (sorted)

    // validCondition (declared by the operation, no tag needed)
    bool conditionChecked;
    std::string conditionExpr;
    bool conditionOk;

    // data region
    std::string byteOrder;                  ///< resolved: tag override -> protocol -> BigEndian
    int dataStartIndex;
    std::size_t frameLength;
    std::size_t rawLength;                  ///< bytes actually fed to the type converter
    std::string rawHex;

    // parsed value
    bool hasValue;
    std::string valueType;                  ///< ValueType name ("Empty" when nothing was parsed)
    std::string valueText;                  ///< human-readable rendering
    bool hasNumeric;
    double numeric;
    std::string quality;                    ///< Good | Bad | Uncertain
    long long timestamp;                    ///< epoch ms (informational)

    // tag-level converter chain (only when the caller supplied one)
    std::vector<Core::ScaleConverter> converters;
    bool hasPreValue;                       ///< true = valueBeforeConversion is meaningful
    std::string preValueType;               ///< type before the chain (usually the raw register type)
    std::string preValueText;
    bool hasPreNumeric;
    double preNumeric;

    ResponsePreviewResult();
};

/// Parse one response frame. Never throws: failures land in `errors` with ok=false.
ResponsePreviewResult PreviewProtocolResponse(const ResponsePreviewRequest& req);

}} // namespace MyProt::Service

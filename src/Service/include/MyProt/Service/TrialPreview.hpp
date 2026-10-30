// src/Service/include/MyProt/Service/TrialPreview.hpp
// Trial render preview — "protocol JSON + operation + variables -> request bytes".
//
// Why: onboarding a protocol currently means editing JSON, running --validate, starting the
// gateway and watching Wireshark. Validation only proves the config is well formed; it says
// nothing about what the bytes will look like. This API renders one frame through the same
// RequestBuilder pipeline the gateway uses (ADR-0012 keeps the two paths on one implementation),
// so the preview is the real request, not a second interpreter.
//
// Boundaries:
//   - read-only: no disk write, no reload, no device I/O;
//   - protocol text goes through the same config entry (comments / BOM tolerated);
//   - deep validation errors block rendering (a frame from an invalid config would mislead);
//   - autoCompute variables (autoIncrement / frameSlice / expr / crc) are resolved at render
//     time and reported as "auto" without a value; derivedLength values are shown because the
//     parameter layer injects them before rendering.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace MyProt { namespace Service {

/// One variable that took part in the render, with its provenance.
struct TrialPreviewVariable {
    std::string name;
    unsigned long long value;   ///< effective value (0 when hasValue == false)
    bool hasValue;              ///< false for autoCompute entries resolved at render time
    std::string source;         ///< protocol | operation | override | default | derived | auto

    TrialPreviewVariable() : value(0), hasValue(false) {}
};

struct TrialPreviewRequest {
    std::string protocolJson;   ///< candidate protocol text (may contain // and /* */ comments)
    std::string operation;      ///< operation map key or op.name
    /// Scalar overrides (highest precedence: tag-like per-request values).
    std::unordered_map<std::string, unsigned int> variables;
    /// {Name:raw} payloads as hex text (no spaces required). Missing ones get a dummy payload.
    std::unordered_map<std::string, std::string> rawVariables;
    /// Dummy payload size used when a raw placeholder has no value (default 3).
    std::size_t payloadBytes;

    TrialPreviewRequest() : payloadBytes(3) {}
};

struct TrialPreviewResult {
    bool ok;
    std::string protocolName;
    std::string operation;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    std::string frameHex;       ///< "50 00 00 FF ..."
    std::string ascii;          ///< printable ASCII, '.' otherwise
    std::size_t frameLength;

    bool hasLengthField;
    int lengthFieldOffset;
    int lengthFieldLength;
    std::string lengthFieldByteOrder;   ///< BigEndian | LittleEndian
    long long lengthFieldValue;         ///< value actually written into the slot
    long long lengthFieldExpected;      ///< value required by the framing semantics
    bool lengthFieldOk;

    std::vector<TrialPreviewVariable> variables;  ///< sorted by name
    std::vector<std::string> operations;          ///< all operation names (sorted)

    TrialPreviewResult();
};

/// Render one request frame. Never throws: all failures land in `errors` with ok=false.
TrialPreviewResult PreviewProtocolOperation(const TrialPreviewRequest& req);

}} // namespace MyProt::Service

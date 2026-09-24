// src/Gateway/include/MyProt/Gateway/MergedRequest.hpp
// Merged request - multiple tags merged into one batch read (modules/05_Gateway.md)

#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace MyProt { namespace Gateway {

/// Merged request - multiple adjacent tags merged into one batch read
/// Fields: startByteAddress / byteCount (cross-protocol byte unit).
///   Byte semantics: the cross-protocol unified address unit; the protocol-family "register = 2 bytes" conversion
///   is expressed by the protocol JSON derivedLength; the engine performs no implicit conversion (hardcoding like tagAddr * 2 is forbidden).
struct MergedRequest {
    std::string deviceId;
    std::string operation;
    uint32_t startByteAddress;    // start byte address (filled after derivation by the protocol derivedLength)
    uint32_t byteCount;           // data-region byte span
    std::vector<size_t> tagIndices; // indices into the original tags array, used to split the response
};

}} // namespace MyProt::Gateway

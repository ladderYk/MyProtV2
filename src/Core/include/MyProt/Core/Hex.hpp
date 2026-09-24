// src/Core/include/MyProt/Core/Hex.hpp
// Hex character utilities - single implementation (previously duplicated once each in the Simulation layer's TemplateMatcher/ResponseSynthesizer).
// C++11; stateless inline, included and reused across modules.
#pragma once

namespace MyProt { namespace Core {

/// Hex character -> numeric value; returns -1 for an invalid character.
inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}} // namespace MyProt::Core

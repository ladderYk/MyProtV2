// src/Core/include/MyProt/Core/ByteView.hpp
// Lightweight byte view - replaces std::span<const uint8_t>, C++11, zero dependencies (ADR-0010 §3)

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MyProt { namespace Core {

using Bytes = std::vector<uint8_t>;

/// Lightweight byte view (replaces std::span<const uint8_t>; ADR-0010 §3). Does not own data;
/// its lifetime is guaranteed by the caller (the channel's internal buffer / request frame stays alive during SendReceive).
struct ByteView {
    const uint8_t* data;
    size_t size;

    ByteView() : data(0), size(0) {}
    ByteView(const uint8_t* d, size_t n) : data(d), size(n) {}
    ByteView(const Bytes& b) : data(b.data()), size(b.size()) {}

    bool empty() const { return size == 0; }
    const uint8_t& operator[](size_t i) const { return data[i]; }
};

}} // namespace MyProt::Core

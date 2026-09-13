// src/Core/include/MyProt/Core/ByteView.hpp
// 轻量字节视图 — 取代 std::span<const uint8_t>, C++11, 零依赖 (ADR-0010 §3)

#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MyProt { namespace Core {

using Bytes = std::vector<uint8_t>;

/// 轻量字节视图 (取代 std::span<const uint8_t>; ADR-0010 §3)。不持有数据,
/// 生命周期由调用方保证 (通道内部缓冲 / 请求帧在 SendReceive 期间存活)。
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

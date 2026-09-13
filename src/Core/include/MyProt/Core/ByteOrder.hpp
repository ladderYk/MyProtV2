// src/Core/include/MyProt/Core/ByteOrder.hpp
// 字节序枚举与转换工具 (C++11, ADR-0010)
// 实现与主机端序无关: 全部位移装配, 不依赖本机内存布局

#pragma once
#include <cstdint>
#include <vector>
#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Core {

enum class ByteOrder {
    BigEndian,          // 标准大端
    LittleEndian,       // 标准小端
    WordBigByteLittle,  // 混合 CDAB: 低字在前, 字内大端 (Melsec)
    WordLittleByteBig   // 混合 BADC: 高字在前, 字内小端
};

// ── 写路径: 数值 → 线上字节 (MSB 在前为下标小) ──

inline std::vector<uint8_t> ToBytes(uint16_t val, ByteOrder order) {
    // 16 位单字无字间序: 除 LittleEndian 外一律按大端输出
    std::vector<uint8_t> r(2);
    if (order == ByteOrder::LittleEndian) {
        r[0] = static_cast<uint8_t>(val);
        r[1] = static_cast<uint8_t>(val >> 8);
    } else {
        r[0] = static_cast<uint8_t>(val >> 8);
        r[1] = static_cast<uint8_t>(val);
    }
    return r;
}

inline std::vector<uint8_t> ToBytes(uint32_t val, ByteOrder order) {
    const uint16_t w0 = static_cast<uint16_t>(val);          // 低 16 位字
    const uint16_t w1 = static_cast<uint16_t>(val >> 16);    // 高 16 位字
    std::vector<uint8_t> r(4);
    switch (order) {
        case ByteOrder::BigEndian:
            r[0] = static_cast<uint8_t>(val >> 24); r[1] = static_cast<uint8_t>(val >> 16);
            r[2] = static_cast<uint8_t>(val >> 8);  r[3] = static_cast<uint8_t>(val);
            break;
        case ByteOrder::LittleEndian:
            r[0] = static_cast<uint8_t>(val);       r[1] = static_cast<uint8_t>(val >> 8);
            r[2] = static_cast<uint8_t>(val >> 16); r[3] = static_cast<uint8_t>(val >> 24);
            break;
        case ByteOrder::WordBigByteLittle:
            // 低字在前, 字内大端: [w0_BE, w1_BE]
            r[0] = static_cast<uint8_t>(w0 >> 8); r[1] = static_cast<uint8_t>(w0);
            r[2] = static_cast<uint8_t>(w1 >> 8); r[3] = static_cast<uint8_t>(w1);
            break;
        case ByteOrder::WordLittleByteBig:
            // 高字在前, 字内小端: [w1_LE, w0_LE]
            r[0] = static_cast<uint8_t>(w1);      r[1] = static_cast<uint8_t>(w1 >> 8);
            r[2] = static_cast<uint8_t>(w0);      r[3] = static_cast<uint8_t>(w0 >> 8);
            break;
    }
    return r;
}

inline std::vector<uint8_t> ToBytes(uint64_t val, ByteOrder order) {
    const uint32_t w0 = static_cast<uint32_t>(val);          // 低 32 位字
    const uint32_t w1 = static_cast<uint32_t>(val >> 32);    // 高 32 位字
    std::vector<uint8_t> r(8);
    switch (order) {
        case ByteOrder::BigEndian:
            for (int i = 7; i >= 0; --i) { r[static_cast<size_t>(7 - i)] = static_cast<uint8_t>(val >> (i * 8)); }
            break;
        case ByteOrder::LittleEndian:
            for (int i = 0; i < 8; ++i) { r[static_cast<size_t>(i)] = static_cast<uint8_t>(val >> (i * 8)); }
            break;
        case ByteOrder::WordBigByteLittle: {
            // 低 32 位字在前, 字组内大端
            const std::vector<uint8_t> b0 = ToBytes(w0, ByteOrder::BigEndian);
            const std::vector<uint8_t> b1 = ToBytes(w1, ByteOrder::BigEndian);
            for (size_t i = 0; i < 4; ++i) { r[i] = b0[i]; r[i + 4] = b1[i]; }
            break;
        }
        case ByteOrder::WordLittleByteBig: {
            // 高 32 位字在前, 字组内小端
            const std::vector<uint8_t> b0 = ToBytes(w0, ByteOrder::LittleEndian);
            const std::vector<uint8_t> b1 = ToBytes(w1, ByteOrder::LittleEndian);
            for (size_t i = 0; i < 4; ++i) { r[i] = b1[i]; r[i + 4] = b0[i]; }
            break;
        }
    }
    return r;
}

// ── 读路径: 线上字节 → 数值 ──

inline uint16_t FromBytesU16(ByteView data, ByteOrder order) {
    if (data.size < 2) return 0;
    uint16_t v = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
    // 16 位单字无字间序: 仅 LittleEndian 需交换
    if (order == ByteOrder::LittleEndian) v = static_cast<uint16_t>(((v & 0x00FFu) << 8) | ((v >> 8) & 0x00FFu));
    return v;
}

inline uint32_t FromBytesU32(ByteView data, ByteOrder order) {
    if (data.size < 4) return 0;
    const uint16_t wa = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]); // 前 2 字节按 BE
    const uint16_t wb = static_cast<uint16_t>((static_cast<uint16_t>(data[2]) << 8) | data[3]); // 后 2 字节按 BE
    switch (order) {
        case ByteOrder::BigEndian:
            return (static_cast<uint32_t>(wa) << 16) | wb;
        case ByteOrder::LittleEndian:
            return (static_cast<uint32_t>(data[3]) << 24) | (static_cast<uint32_t>(data[2]) << 16) |
                   (static_cast<uint32_t>(data[1]) << 8)  | data[0];
        case ByteOrder::WordBigByteLittle:
            // 低字在前, 字内大端
            return (static_cast<uint32_t>(wb) << 16) | wa;
        case ByteOrder::WordLittleByteBig: {
            // 高字在前, 字内小端: 反转每字内字节序后重组
            const uint16_t w1 = static_cast<uint16_t>(((wa & 0x00FFu) << 8) | ((wa >> 8) & 0x00FFu));
            const uint16_t w0 = static_cast<uint16_t>(((wb & 0x00FFu) << 8) | ((wb >> 8) & 0x00FFu));
            return (static_cast<uint32_t>(w1) << 16) | w0;
        }
    }
    return 0;
}

inline uint64_t FromBytesU64(ByteView data, ByteOrder order) {
    if (data.size < 8) return 0;
    if (order == ByteOrder::BigEndian) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | data[i];
        return v;
    }
    if (order == ByteOrder::LittleEndian) {
        uint64_t v = 0;
        for (int i = 7; i >= 0; --i) v = (v << 8) | data[i];
        return v;
    }
    // 混合序: 拆为两个 32 位字组
    const ByteView ha(data.data, 4);
    const ByteView hb(data.data + 4, 4);
    if (order == ByteOrder::WordBigByteLittle) {
        // 低 32 位字在前, 字组内大端
        const uint32_t v0 = FromBytesU32(ha, ByteOrder::BigEndian);
        const uint32_t v1 = FromBytesU32(hb, ByteOrder::BigEndian);
        return (static_cast<uint64_t>(v1) << 32) | v0;
    }
    // 高 32 位字在前, 字组内小端
    const uint32_t v1 = FromBytesU32(ha, ByteOrder::LittleEndian);
    const uint32_t v0 = FromBytesU32(hb, ByteOrder::LittleEndian);
    return (static_cast<uint64_t>(v1) << 32) | v0;
}

}} // namespace MyProt::Core

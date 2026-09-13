// src/Core/include/MyProt/Core/Hex.hpp
// 十六进制字符工具 — 单一实现 (原 Simulation 层 TemplateMatcher/ResponseSynthesizer 各一份)。
// C++11; 内联无状态, 供各模块 include 复用。
#pragma once

namespace MyProt { namespace Core {

/// 十六进制字符 → 数值; 非法字符返回 -1。
inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}} // namespace MyProt::Core

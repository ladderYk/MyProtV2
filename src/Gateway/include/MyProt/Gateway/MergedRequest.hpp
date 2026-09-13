// src/Gateway/include/MyProt/Gateway/MergedRequest.hpp
// 合并请求 — 多个标签合并为一次批量读取 (modules/05_Gateway.md)

#pragma once
#include <vector>
#include <string>
#include <cstdint>

namespace MyProt { namespace Gateway {

/// 合并请求 — 多个相邻标签合并为一次批量读取
/// v1.25 改: startAddress/totalSpan → startByteAddress/byteCount.
///   字节语义: 跨协议统一的地址单位; 协议族"寄存器 = 2 字节"换算由协议 JSON derivedLength 表达,
///   引擎不再做隐式换算 (tagAddr * 2 这类硬编码已移除).
struct MergedRequest {
    std::string deviceId;
    std::string operation;
    uint32_t startByteAddress;    // 起始字节地址 (按协议 derivedLength 派生后填入)
    uint32_t byteCount;           // 数据区字节跨度
    std::vector<size_t> tagIndices; // 原始 tags 数组中的索引, 用于拆分响应
};

}} // namespace MyProt::Gateway

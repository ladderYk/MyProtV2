// src/Core/include/MyProt/Core/Value.hpp
// 值类型系统 — TypedValue, TagValue, QualityCode (C++11, ADR-0010 §3)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Core {

enum class QualityCode { Good, Bad, Uncertain };

enum class ValueType {
    Empty,      // 空 (Bad/Uncertain 质量时)
    ByteArray,
    UInt16, Int16, UInt32, Int32, UInt64, Int64,
    Float, Double,
    Bool,
    String
};

/// 类型化值 — tagged union (取代 std::variant, ADR-0010 §3)。
/// type 决定读取哪个存储字段; 整型统一加宽存放 (UInt16→u, Int16→i), float 加宽为 double (无损)。
struct TypedValue {
    ValueType type;
    std::vector<uint8_t> bytes;   // type == ByteArray
    uint64_t u;                   // type == UInt16/UInt32/UInt64
    int64_t i;                    // type == Int16/Int32/Int64
    double d;                     // type == Float/Double
    bool b;                       // type == Bool
    std::string str;              // type == String

    TypedValue()
        : type(ValueType::Empty), u(0), i(0), d(0.0), b(false) {}
};

struct TagValue {
    std::string tagName;
    std::string deviceId;
    TypedValue typedValue;
    std::vector<uint8_t> rawData;       // Uncertain 时保留原始字节供诊断 (ADR-0006)
    QualityCode quality;                // 失败时按 ADR-0006 语义置 Bad / Uncertain
    Error lastError;
    int64_t timestamp;                  // epoch ms
    int64_t requestId;                  // 端到端关联 id (correlation id, ADR-0009)
    bool valueChanged;                  // 相比上次是否有变化

    TagValue()
        : quality(QualityCode::Good), timestamp(0), requestId(0), valueChanged(false) {}
};

}} // namespace MyProt::Core

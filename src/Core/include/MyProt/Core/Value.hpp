// src/Core/include/MyProt/Core/Value.hpp
// Value type system - TypedValue, TagValue, QualityCode (C++11, ADR-0010 §3)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Core {

enum class QualityCode { Good, Bad, Uncertain };

enum class ValueType {
    Empty,      // empty (when quality is Bad/Uncertain)
    ByteArray,
    UInt16, Int16, UInt32, Int32, UInt64, Int64,
    Float, Double,
    Bool,
    String
};

/// Typed value - tagged union (replaces std::variant, ADR-0010 §3).
/// type decides which storage field to read; integers are uniformly widened (UInt16→u, Int16→i), float widened to double (lossless).
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

/// Type + payload equality (ignores quality, timestamp and request id).
/// Single definition shared by the value store and northbound filters so that
/// "changed" means the same thing on every path. Float/Double compare by value,
/// so NaN never equals NaN (a NaN sample always counts as changed).
inline bool TypedValueEquals(const TypedValue& a, const TypedValue& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case ValueType::Empty:     return true;
    case ValueType::ByteArray: return a.bytes == b.bytes;
    case ValueType::UInt16:
    case ValueType::UInt32:
    case ValueType::UInt64:    return a.u == b.u;
    case ValueType::Int16:
    case ValueType::Int32:
    case ValueType::Int64:     return a.i == b.i;
    case ValueType::Float:
    case ValueType::Double:    return a.d == b.d;
    case ValueType::Bool:      return a.b == b.b;
    case ValueType::String:    return a.str == b.str;
    default:                   return false;
    }
}

struct TagValue {
    std::string tagName;
    std::string deviceId;
    TypedValue typedValue;
    std::vector<uint8_t> rawData;       // on Uncertain, keeps the raw bytes for diagnostics (ADR-0006)
    QualityCode quality;                // on failure, set Bad / Uncertain per ADR-0006 semantics
    Error lastError;
    int64_t timestamp;                  // epoch ms
    int64_t requestId;                  // end-to-end correlation id (ADR-0009)
    bool valueChanged;                  // derived by LatestValueStore::Update (not by producers)

    TagValue()
        : quality(QualityCode::Good), timestamp(0), requestId(0), valueChanged(false) {}
};

}} // namespace MyProt::Core

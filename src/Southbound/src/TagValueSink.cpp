// src/Southbound/src/TagValueSink.cpp

#include "MyProt/Southbound/TagValueSink.hpp"

#include <chrono>
#include <cmath>

namespace MyProt { namespace Southbound {

namespace {

bool IsNumeric(const Core::TypedValue& v) {
    switch (v.type) {
    case Core::ValueType::UInt16:
    case Core::ValueType::UInt32:
    case Core::ValueType::UInt64:
    case Core::ValueType::Int16:
    case Core::ValueType::Int32:
    case Core::ValueType::Int64:
    case Core::ValueType::Float:
    case Core::ValueType::Double:
        return true;
    default:
        return false;
    }
}

double AsDouble(const Core::TypedValue& v) {
    switch (v.type) {
    case Core::ValueType::UInt16:
    case Core::ValueType::UInt32:
    case Core::ValueType::UInt64:
        return static_cast<double>(v.u);
    case Core::ValueType::Int16:
    case Core::ValueType::Int32:
    case Core::ValueType::Int64:
        return static_cast<double>(v.i);
    case Core::ValueType::Float:
    case Core::ValueType::Double:
        return v.d;
    default:
        return 0.0;
    }
}

} // namespace

TagValueSink::TagValueSink(StoreWriter writer) : _writer(writer) {}

void TagValueSink::Reset() {
    std::lock_guard<std::mutex> lock(_mutex);
    _last.clear();
}

int64_t TagValueSink::NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool TagValueSink::SameValue(const Core::TypedValue& a, const Core::TypedValue& b) {
    return Core::TypedValueEquals(a, b);
}

Core::TypedValue TagValueSink::ApplyConverters(const Core::TagDefinition& tag,
                                               const Core::TypedValue& in) const {
    if (tag.converters.empty() || !IsNumeric(in)) return in;
    double v = AsDouble(in);
    for (size_t i = 0; i < tag.converters.size(); ++i) {
        v = v * tag.converters[i].k + tag.converters[i].b;
    }
    Core::TypedValue out;
    out.type = Core::ValueType::Double;
    out.d = v;
    return out;
}

void TagValueSink::Emit(const Core::TagDefinition& tag,
                        const Core::TypedValue& value,
                        int64_t timestampMs,
                        const std::vector<uint8_t>& raw) {
    Core::TagValue tv;
    tv.tagName = tag.name;
    tv.deviceId = tag.deviceId;
    tv.typedValue = ApplyConverters(tag, value);
    tv.timestamp = timestampMs > 0 ? timestampMs : NowMs();
    tv.quality = Core::QualityCode::Good;
    tv.rawData = raw;
    tv.requestId = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _last[tag.name] = tv.typedValue;   // baseline for EmitSilent only
    }
    // valueChanged 由 LatestValueStore::Update 统一计算 (见 Value.hpp)
    if (_writer) {
        std::vector<Core::TagValue> one(1, tv);
        _writer(one);
    }
}

void TagValueSink::EmitBad(const Core::TagDefinition& tag,
                           const std::string& reason,
                           int64_t timestampMs,
                           const std::vector<uint8_t>& raw) {
    Core::TagValue tv;
    tv.tagName = tag.name;
    tv.deviceId = tag.deviceId;
    tv.timestamp = timestampMs > 0 ? timestampMs : NowMs();
    tv.quality = Core::QualityCode::Bad;
    tv.rawData = raw;
    tv.requestId = 0;
    tv.lastError = Core::Error::Make(
        Core::Error::Code::ParseError, reason,
        "device=" + tag.deviceId + ", tag=" + tag.name);
    // 质量变化由 LatestValueStore 检测, 这里不自行置变化位
    if (_writer) {
        std::vector<Core::TagValue> one(1, tv);
        _writer(one);
    }
}

void TagValueSink::EmitSilent(const Core::TagDefinition& tag, int64_t timestampMs) {
    Core::TagValue tv;
    tv.tagName = tag.name;
    tv.deviceId = tag.deviceId;
    tv.timestamp = timestampMs > 0 ? timestampMs : NowMs();
    tv.quality = Core::QualityCode::Uncertain;
    tv.requestId = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::map<std::string, Core::TypedValue>::const_iterator it =
            _last.find(tag.name);
        if (it != _last.end()) tv.typedValue = it->second;
    }
    // 值不变、质量 Good→Uncertain, 由 LatestValueStore 判为变化
    if (_writer) {
        std::vector<Core::TagValue> one(1, tv);
        _writer(one);
    }
}

}} // namespace MyProt::Southbound

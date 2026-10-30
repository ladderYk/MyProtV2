// src/Southbound/include/MyProt/Southbound/TagValueSink.hpp
// Single write entry for device-initiated values. Polling results go straight to
// LatestValueStore; push adapters must go through this sink so that quality
// degradation and converter application stay identical. Change detection is NOT
// done here: LatestValueStore::Update derives TagValue::valueChanged for both paths.

#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Southbound {

class TagValueSink {
public:
    typedef std::function<void(const std::vector<Core::TagValue>&)> StoreWriter;

    explicit TagValueSink(StoreWriter writer);

    /// A successfully decoded value (quality = Good).
    void Emit(const Core::TagDefinition& tag,
              const Core::TypedValue& value,
              int64_t timestampMs,
              const std::vector<uint8_t>& raw);

    /// A decode failure (quality = Bad, typedValue cleared, raw kept for diagnostics).
    void EmitBad(const Core::TagDefinition& tag,
                 const std::string& reason,
                 int64_t timestampMs,
                 const std::vector<uint8_t>& raw);

    /// No frame within maxSilenceMs (quality = Uncertain, last good value kept).
    void EmitSilent(const Core::TagDefinition& tag, int64_t timestampMs);

    /// Drop the last-good-value cache (called on config reload; the store is cleared too).
    void Reset();

    static int64_t NowMs();

    /// Type + value equality (delegates to Core::TypedValueEquals, kept for callers).
    static bool SameValue(const Core::TypedValue& a, const Core::TypedValue& b);

private:
    Core::TypedValue ApplyConverters(const Core::TagDefinition& tag,
                                     const Core::TypedValue& in) const;

    StoreWriter _writer;
    mutable std::mutex _mutex;
    std::map<std::string, Core::TypedValue> _last;   // last good value per tag (EmitSilent fallback)

    TagValueSink(const TagValueSink&) = delete;
    TagValueSink& operator=(const TagValueSink&) = delete;
};

}} // namespace MyProt::Southbound

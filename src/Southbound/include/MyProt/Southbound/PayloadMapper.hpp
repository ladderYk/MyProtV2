// src/Southbound/include/MyProt/Southbound/PayloadMapper.hpp
// Inbound payload decoding: topic / JSON payload -> (tagName, TypedValue).
// Binary payloads are out of scope in v1: an inbound frame must carry JSON.

#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Southbound {

struct MappingItem {
    std::string tagName;
    nlohmann::json value;       // decoded after the tag (and its finalType) is known
    int64_t timestampMs;        // 0 = use the frame receive time

    MappingItem() : timestampMs(0) {}
};

class PayloadMapper {
public:
    /// Topic segment (1-based) -> tag name, e.g. "plant/PLC-1/Temp" with index=3.
    static bool MapTopicTag(const std::string& topic, int index1Based, std::string& out);

    /// Decode a JSON payload.
    /// - object with valueField: one item; tag name from tagField (or defaultTag)
    /// - other object: batch map { "TagA": 1.5, "TagB": 2 } (defaultTag ignored)
    /// - scalar / array: one item named defaultTag (empty defaultTag = error)
    static bool MapJson(const std::string& payload,
                        const std::string& tagField,
                        const std::string& valueField,
                        const std::string& tsField,
                        const std::string& defaultTag,
                        std::vector<MappingItem>& out,
                        std::string& err);

    /// Convert a JSON scalar to the tag's finalType ("Auto" = infer from JSON).
    static bool ToTypedValue(const nlohmann::json& value,
                             const std::string& finalType,
                             Core::TypedValue& out,
                             std::string& err);
};

}} // namespace MyProt::Southbound

// src/Polling/include/MyProt/Polling/LatestValueStore.hpp
// Latest-value snapshot store - keeps each tag's most recent acquisition result, queried by /api/data/latest
// Thread safety: written by the PollingEngine io thread (Update), read by the WebApi thread (Snapshot)

#pragma once
#include <map>
#include <string>
#include <vector>
#include <mutex>

#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Polling {

class LatestValueStore {
public:
    /// Batch-update the latest values (called on the PollingEngine callback thread; for same-named tags a later write overrides an earlier one)
    void Update(const std::vector<Core::TagValue>& values);

    /// Full snapshot (sorted by tagName); when deviceFilter is non-empty, filter by deviceId prefix
    std::vector<Core::TagValue> Snapshot(
        const std::string& deviceFilter = std::string()) const;

        /// Single-tag lookup: while a device is offline, use the last value instead of emitting Bad
    /// @return true and out filled - present; false - the tag never had a value
    bool Lookup(const std::string& tagName, Core::TagValue& out) const;

    /// Current tag count
    size_t Count() const;

    /// Clear the snapshot (called after a config hot reload, discarding stale data of the old tag set)
    void Clear();

private:
    mutable std::mutex _mutex;
    std::map<std::string, Core::TagValue> _latest;   // tagName -> latest value
};

}} // namespace MyProt::Polling

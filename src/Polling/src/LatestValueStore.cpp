// src/Polling/src/LatestValueStore.cpp
// 最新值快照存储 — 实现见 LatestValueStore.hpp

#include "MyProt/Polling/LatestValueStore.hpp"

namespace MyProt { namespace Polling {

void LatestValueStore::Update(const std::vector<Core::TagValue>& values) {
    std::lock_guard<std::mutex> lock(_mutex);
    for (size_t i = 0; i < values.size(); ++i) {
        _latest[values[i].tagName] = values[i];
    }
}

std::vector<Core::TagValue> LatestValueStore::Snapshot(
        const std::string& deviceFilter) const {
    std::vector<Core::TagValue> out;
    std::lock_guard<std::mutex> lock(_mutex);
    out.reserve(_latest.size());
    for (std::map<std::string, Core::TagValue>::const_iterator it =
                _latest.begin(); it != _latest.end(); ++it) {
        if (!deviceFilter.empty() && it->second.deviceId != deviceFilter) continue;
        out.push_back(it->second);
    }
    return out;
}

size_t LatestValueStore::Count() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _latest.size();
}

bool LatestValueStore::Lookup(const std::string& tagName, Core::TagValue& out) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _latest.find(tagName);
    if (it == _latest.end()) return false;
    out = it->second;
    return true;
}

void LatestValueStore::Clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _latest.clear();
}

}} // namespace MyProt::Polling

// src/Polling/include/MyProt/Polling/PollStats.hpp
// Poll statistics - single point of definition; a standalone header for lightweight external reference (avoids including the full PollingEngine.hpp)

#pragma once
#include <atomic>
#include <cstdint>

namespace MyProt { namespace Polling {

/// Poll statistics (atomic, thread-safe)
struct PollStats {
    std::atomic<int64_t> totalReads{0};
    std::atomic<int64_t> activeTags{0};
    // Cumulative poll groups built since construction. ApplySnapshot only rebuilds the groups of
    // devices whose poll signature changed, so this counter's growth across a hot reload is the
    // direct measure of diff fidelity (a config save should not churn the whole fleet).
    std::atomic<int64_t> groupsBuilt{0};
};

}} // namespace MyProt::Polling

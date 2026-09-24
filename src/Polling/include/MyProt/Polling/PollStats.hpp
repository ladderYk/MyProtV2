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
};

}} // namespace MyProt::Polling

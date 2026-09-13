// src/Polling/include/MyProt/Polling/PollStats.hpp
// 轮询统计 — 唯一定义处; 独立头文件供外部轻量引用 (避免包含完整 PollingEngine.hpp)

#pragma once
#include <atomic>
#include <cstdint>

namespace MyProt { namespace Polling {

/// 轮询统计 (atomic, 多线程安全)
struct PollStats {
    std::atomic<int64_t> totalReads{0};
    std::atomic<int64_t> activeTags{0};
};

}} // namespace MyProt::Polling

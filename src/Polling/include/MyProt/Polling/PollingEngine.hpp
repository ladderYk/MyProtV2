// src/Polling/include/MyProt/Polling/PollingEngine.hpp
// 轮询引擎 — 按 scanRateMs 分组定时调度标签读取 (modules/06_Polling.md, ADR-0004)
// C++11 回调模型 (ADR-0010), 无协程

#pragma once
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <functional>
#include <unordered_map>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Polling/PollStats.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"

namespace MyProt { namespace Polling {

/// 结果分发回调: 每轮读取完成后调用, 参数为本轮所有 TagValue (含 Bad 质量)
using ResultDispatch = std::function<void(const std::vector<Core::TagValue>&)>;

/// 轮询引擎 — 按 scanRateMs 定时调度各设备的标签读取
/// 依赖: Gateway::ProtocolGateway (TagGrouper + TagReader + ChannelManager)
/// 调度: 每组一个 asio::steady_timer, 回调链驱动异步读取
/// 背压: 上一批未完成 → 跳过新批次 (ADR-0004 §deadline)
/// Deadline: 每轮读取预算 = scanRateMs, 超时标 Bad 不级联
class PollingEngine {
public:
    PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway);

    /// 启动轮询
    /// @param tags  全部标签定义 (拷贝存储, 按 scanRateMs 分组)
    /// @param devices  设备配置 (用于构建 deviceId → protocolName 映射)
    /// @param onResults  每轮读取完成后的结果分发回调 (可选)
    /// @param globalResilience  全局韧性策略 (ConfigRoot.resilience); 设备级
    ///                          resilience 覆盖之, 均未设置时用内置默认
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// 停止轮询 (取消所有定时器, 在飞批次自然完成)
    void Stop();

    /// 获取统计信息
    const PollStats& GetStats() const { return _stats; }

    /// 是否正在运行
    bool IsRunning() const { return _running.load(); }

private:
    /// 轮询组 — 同一 scanRateMs 的所有标签 + 合并请求
    struct PollGroup {
        int scanRateMs;
        std::uint64_t generation;   // 装配代际 — 热重载后旧代 in-flight 组不得续排
        std::vector<Core::TagDefinition> allTags;      // 原始标签数组 (tagIndices 引用此数组)
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;     // 背压标志
        // 本轮总预算截止时刻 (= 触发时刻 + scanRateMs, ADR-0004 §3);
        // 重试/重连消耗同一预算, 超预算不级联到下一周期
        std::chrono::steady_clock::time_point batchDeadline;

        PollGroup(asio::io_context& io)
            : scanRateMs(1000), generation(0), timer(io), busy(false) {}
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    std::atomic<std::uint64_t> _generation;  // 当前装配代际 (Start 时递增)
    ResultDispatch _onResults;   // 结果分发回调
    std::unordered_map<std::string, std::string> _deviceProtocolMap;  // deviceId → protocolName
    std::unordered_map<std::string, int> _deviceTimeoutMap;           // deviceId → requestTimeoutMs
    std::unordered_map<std::string, Core::ResilienceConfig>
        _deviceResilienceMap;    // deviceId → 生效韧性 (设备级覆盖 > 全局 > 默认)
    std::vector<std::shared_ptr<PollGroup>> _groups;

    /// 调度下一轮定时器
    void ScheduleNext(std::shared_ptr<PollGroup> group);

    /// 定时器触发回调
    void OnTimerFire(std::shared_ptr<PollGroup> group,
                     const asio::error_code& ec);

    /// 处理设备的一轮读取 (协议查找 → 通道获取 → ReadBatch 链)
    /// attempt: 当前连接尝试序号 (1 起); 连接类可重试故障按韧性策略退避后重入
    void ProcessDevice(std::shared_ptr<PollGroup> group, size_t deviceIdx,
                       std::shared_ptr<std::vector<Core::TagValue>> results,
                       int attempt);

    /// 链式执行各 MergedRequest 的 ReadBatch
    /// deviceIdx: 设备段起始索引; curIdx: 当前请求索引 (同段内递增)
    /// attempt: 当前读尝试序号 (1 起); 可重试故障退避后重试同一请求
    void PollBatchChain(std::shared_ptr<PollGroup> group,
                        size_t deviceIdx,
                        size_t curIdx,
                        Gateway::TagReader* reader,
                        std::shared_ptr<Core::ProtocolConfig> protocol,
                        std::shared_ptr<Transport::IChannel> channel,
                        std::shared_ptr<Service::SessionContext> session,
                        std::shared_ptr<std::vector<Core::TagValue>> allResults,
                        int attempt);

    /// 一轮读取完成: 更新统计, 分发结果, 解除背压, 调度下一轮
    void FinishBatch(std::shared_ptr<PollGroup> group,
                     std::shared_ptr<std::vector<Core::TagValue>> results);

    /// 丢弃旧代批次 (热重载后 generation 不匹配): 不分发结果、不更新统计,
    /// 仅释放背压 — 防已删除/改名标签的陈旧值复活到新数据面。
    void AbandonBatch(std::shared_ptr<PollGroup> group);

    /// 将指定 MergedRequest 的所有标签标记为 Bad, 追加到 results
    void AppendBadValues(std::shared_ptr<PollGroup> group,
                         size_t deviceIdx,
                         const Core::Error& error,
                         std::vector<Core::TagValue>& results);

    /// 设备级失败: 该设备段所有 MergedRequest 的标签标 Bad (同设备请求按
    /// deviceId 连续存放) 后推进到下一设备; 不做熔断记账, 由调用方决定
    void FailDevice(std::shared_ptr<PollGroup> group,
                    size_t deviceIdx,
                    const Core::Error& error,
                    std::shared_ptr<std::vector<Core::TagValue>> results);

    /// 通过 deviceId 查找协议配置 (经 _deviceProtocolMap + ProtocolLookup)
    std::shared_ptr<Core::ProtocolConfig>
    GetProtocolForDevice(const std::string& deviceId);

    /// 生效韧性配置: 设备级覆盖 > 全局 > 内置默认 (struct 仅 6 int, 按值返回)
    Core::ResilienceConfig ResilienceFor(const std::string& deviceId) const;

    /// 本轮剩余预算(ms) = batchDeadline - now; 已耗尽返回 0
    int RemainingBudgetMs(std::shared_ptr<PollGroup> group) const;

    /// 指数退避: min(backoffMaxMs, backoffBaseMs × 2^(failedAttempt-1))
    static int BackoffDelayMs(const Core::ResilienceConfig& res,
                              int failedAttempt);

    /// 退避等待后继续 (独立临时 timer, 不占用组调度 timer);
    /// cont 在 io 线程执行, 其内部须自行校验 _stopping/代际
    void RetryAfterBackoff(std::shared_ptr<PollGroup> group, int delayMs,
                           const std::function<void()>& cont);
};

}} // namespace MyProt::Polling

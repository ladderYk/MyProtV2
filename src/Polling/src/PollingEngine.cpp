// src/Polling/src/PollingEngine.cpp
// 轮询引擎实现 — asio::steady_timer 分组调度 + 异步回调链 (C++11, ADR-0010)
// 数据流: timer → 断路器检查 → GetOrCreateChannel → ReadBatch 链 → 结果分发
// 背压: 上批未完成 → 跳过本轮
// Deadline: 每轮预算 = scanRateMs; 第 k 次尝试有效超时 = min(requestTimeoutMs,
//           剩余预算); 重试/重连消耗同一预算, 超时标 Bad 不级联 (ADR-0004 §3)
// 重试: 读操作可重试故障 (IsRetryable) 按韧性策略指数退避后重试;
//       预算/maxAttempts 耗尽 → RecordFailure (计入熔断) + Bad Quality

#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Service/SessionContext.hpp"
#include "MyProt/Service/DeviceLifecycle.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Core/Expected.hpp"
#include <algorithm>
#include <map>

namespace MyProt { namespace Polling {

PollingEngine::PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway)
    : _io(io)
    , _gateway(gateway)
    , _stats()
    , _running(false)
    , _stopping(false)
    , _generation(0) {}

// ──────────────────── 生命周期 ────────────────────

void PollingEngine::Start(const std::vector<Core::TagDefinition>& tags,
                          const std::vector<Core::DeviceConfig>& devices,
                          ResultDispatch onResults,
                          const Core::Optional<Core::ResilienceConfig>&
                              globalResilience) {
    if (_running.load()) return;

    _stopping.store(false);
    _running.store(true);
    _generation.fetch_add(1);   // 新装配代际 — 旧代 in-flight 组不得再续排 (热重载防护)
    _groups.clear();
    _protocolCache.clear();     // 协议快照随重装配刷新 (热重载后取新配置)
    _onResults = std::move(onResults);

    // 构建 deviceId → protocolName / requestTimeoutMs / 生效韧性 映射
    // (超时收敛: 2026-08-24; 韧性解析: 设备级覆盖 > 全局 > 内置默认)
    for (const auto& dev : devices) {
        _deviceProtocolMap[dev.id] = dev.protocol;
        _deviceTimeoutMap[dev.id] = dev.requestTimeoutMs;
        if (dev.resilience.has_value()) {
            _deviceResilienceMap[dev.id] = dev.resilience.value();
        } else if (globalResilience.has_value()) {
            _deviceResilienceMap[dev.id] = globalResilience.value();
        } else {
            _deviceResilienceMap[dev.id] = Core::ResilienceConfig();
        }
    }

    // 按 scanRateMs 分组 (TagGrouper::GroupByScanRate)
        // 写标签 (direction=write) 不参与轮询 — 它们只作为 /api/data/write
    //   的定向目标, 无采集语义; 在引擎入口过滤, TagGrouper 无需感知.
    std::vector<Core::TagDefinition> pollTags;
    pollTags.reserve(tags.size());
    for (size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].direction != "write") pollTags.push_back(tags[i]);
    }

    auto tagGroups = _gateway.GetTagGrouper().GroupByScanRate(pollTags);

    // 标签表全部组共享一份 (原实现每组拷贝全量 pollTags — O(组数×标签数) 内存);
    // in-flight 异步链经 PollGroup shared_ptr 持有旧表快照, 热重载替换不悬垂
    auto sharedTags = std::make_shared<const std::vector<Core::TagDefinition>>(
        std::move(pollTags));

    // 设备级 PollGroup: 以 (scanRateMs, deviceId) 为键 — 每设备独立 timer +
    // 独立 deadline 预算, 避免同扫描周期下某设备连接故障重试耗尽共享预算,
    // 拖累其他设备 (ADR-0004 预算模型由"组级"细化为"设备级")。
    // 单设备组内 ProcessDevice/PollBatchChain 串行链自然退化 (换设备分支不再触发)。
    std::map<std::pair<int, std::string>, std::shared_ptr<PollGroup>> groupMap;
    for (const auto& tg : tagGroups) {
        auto key = std::make_pair(tg.scanRateMs, tg.deviceId);
        auto& pg = groupMap[key];   // 缺省构造 shared_ptr (nullptr)
        if (!pg) {
            pg = std::make_shared<PollGroup>(_io);
            pg->scanRateMs = tg.scanRateMs;
            pg->generation = _generation.load();
            pg->tags = sharedTags;
        }
                // 合并跨度来自协议级配置 (不能写死 — 早期写死的 125 语义是 Modbus 的
                //   寄存器上限, 且地址单位改为字节后该值未同步,
        //   实际只剩 125 字节 ≈ 62 寄存器, 合并能力静默缩水一半).
        //   协议查找失败时回退 Core::kDefaultMaxSpanBytes.
        int maxSpan = Core::kDefaultMaxSpanBytes;
        {
            std::shared_ptr<const Core::ProtocolConfig> proto =
                GetProtocolForDevice(tg.deviceId);
            if (proto && proto->maxSpanBytes > 0) maxSpan = proto->maxSpanBytes;
        }
        auto merged = _gateway.GetTagGrouper().CoalesceAdjacent(tg, *sharedTags,
                                                                maxSpan);
        pg->mergedRequests.insert(
            pg->mergedRequests.end(), merged.begin(), merged.end());
    }

    for (auto& pair : groupMap) {
        _groups.push_back(pair.second);
    }

    _stats.activeTags.store(static_cast<int64_t>(sharedTags->size()));

    for (auto& pg : _groups) {
        ScheduleNext(pg);
    }
}

void PollingEngine::Stop() {
    _stopping.store(true);
    _running.store(false);
    for (auto& pg : _groups) {
        asio::error_code ec;
        pg->timer.cancel(ec);
    }
    _groups.clear();
}

// ──────────────────── 定时器调度 ────────────────────

void PollingEngine::ScheduleNext(std::shared_ptr<PollGroup> group) {
    // 代际防护: Stop→Start 连续调用时 (_stopping 已复位), 旧代 in-flight 组
    // 的 FinishBatch 会走到这里 — 代际不匹配直接丢弃, 防旧标签集复活
    if (_stopping.load() || group->generation != _generation.load()) return;
    group->timer.expires_after(std::chrono::milliseconds(group->scanRateMs));
    group->timer.async_wait(
        [this, group](const asio::error_code& ec) {
            OnTimerFire(group, ec);
        });
}

void PollingEngine::OnTimerFire(std::shared_ptr<PollGroup> group,
                                const asio::error_code& ec) {
    if (_stopping.load()) return;
    if (ec) return;  // cancelled
    if (group->generation != _generation.load()) return;  // 旧代组

    // 背压: 上一批未完成 → 跳过
    if (group->busy) {
        ScheduleNext(group);   // 背压: 上一批未完成, 跳过本轮
        return;
    }
    group->busy = true;

    // 本轮 deadline 预算起点 (ADR-0004 §3): 重试/重连共享 scanRateMs 预算,
    // 耗尽即标 Bad, 不级联到下一周期
    group->batchDeadline = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(group->scanRateMs);

    if (group->mergedRequests.empty()) {
        group->busy = false;
        ScheduleNext(group);
        return;
    }

    auto results = std::make_shared<std::vector<Core::TagValue>>();
    ProcessDevice(group, 0, results, /*attempt=*/1);
}

// ──────────────────── 设备处理链 ────────────────────

void PollingEngine::ProcessDevice(
        std::shared_ptr<PollGroup> group,
        size_t deviceIdx,
        std::shared_ptr<std::vector<Core::TagValue>> results,
        int attempt) {

    if (_stopping.load() || deviceIdx >= group->mergedRequests.size()) {
        FinishBatch(group, results);
        return;
    }

    const auto& merged = group->mergedRequests[deviceIdx];
    auto& chMgr = _gateway.GetChannelManager();

    // 检查设备已注册 (无 Session 可记账 — 仅标 Bad)
    auto session = chMgr.GetSession(merged.deviceId);
    if (!session) {
        FailDevice(group, deviceIdx,
            Core::Error{Core::Error::Code::DeviceNotFound,
                "Device not registered", {}}, results);
        return;
    }

    // 断路器检查 (ADR-0004): 已开 → 不发起尝试; 不再 RecordFailure
    // (开启方已记账), 避免重复计数
    if (!session->CanProceed()) {
        FailDevice(group, deviceIdx,
            Core::Error{Core::Error::Code::CircuitOpen,
                "Circuit breaker open", {}}, results);
        return;
    }

    // 异步获取通道 (连接类可重试故障按韧性策略退避后重试)
    chMgr.GetOrCreateChannel(merged.deviceId,
        [this, group, deviceIdx, session, results, attempt](
            Core::Expected<Gateway::ConnectResult> result) {

            if (_stopping.load()) {
                FinishBatch(group, results);
                return;
            }
            if (group->generation != _generation.load()) {
                AbandonBatch(group);   // 旧代: 丢弃, 不复活陈旧标签值
                return;
            }

            if (!result.has_value()) {
                const Core::Error err = result.error();
                const Core::ResilienceConfig res =
                    ResilienceFor(group->mergedRequests[deviceIdx].deviceId);

                // 可重试 + 未耗尽 maxAttempts + 预算未耗尽 → 退避重试
                const bool canRetry = Core::IsRetryable(err.code) &&
                                      attempt < res.maxAttempts &&
                                      RemainingBudgetMs(group) > 0;
                if (canRetry) {
                    const int delayMs = BackoffDelayMs(res, attempt);
                    RetryAfterBackoff(group, delayMs,
                        [this, group, deviceIdx, results, attempt]() {
                            if (_stopping.load()) {
                                FinishBatch(group, results);
                                return;
                            }
                            if (group->generation != _generation.load()) {
                                AbandonBatch(group);
                                return;
                            }
                            ProcessDevice(group, deviceIdx, results,
                                          attempt + 1);
                        });
                    return;
                }
                // 耗尽/不可重试 → 记入熔断, 整设备段标 Bad, 推进下一设备
                session->RecordFailure();
                // KI-04: 连接级故障 → 设备进入 Degraded (与 RecordFailure 正交, 记入熔断
                //  是流量层, 这里是设备层; 下次 timer 触发 GOC 重新尝试连接)。
                // 协议级 (InvalidResponse/ParseError/TagNotFound/...) 不动 lifecycle。
                if (Core::IsLifecycleDegrading(err.code)) {
                    session->SetLifecycle(
                        Service::DeviceLifecycleState::Degraded);
                }
                FailDevice(group, deviceIdx, err, results);
                return;
            }

            auto channel = result.value().channel;

            // 查找协议配置 (配置缺失非连接故障, 不计入熔断)
            auto protocol = GetProtocolForDevice(
                group->mergedRequests[deviceIdx].deviceId);
            if (!protocol) {
                FailDevice(group, deviceIdx,
                    Core::Error{Core::Error::Code::ProtocolNotFound,
                        "Protocol config not found", {}}, results);
                return;
            }

            // 启动链式批量读取 (使用 Gateway 的 TagReader, 生命周期由 ProtocolGateway 保证)
            auto* reader = &_gateway.GetTagReader();

            PollBatchChain(group, deviceIdx, /*curIdx=*/deviceIdx, reader,
                           protocol, channel, session, results,
                           /*attempt=*/1);
        });
}

// ──────────────────── 链式 ReadBatch ────────────────────

void PollingEngine::PollBatchChain(
        std::shared_ptr<PollGroup> group,
        size_t deviceIdx,
        size_t curIdx,
        Gateway::TagReader* reader,
        std::shared_ptr<const Core::ProtocolConfig> protocol,
        std::shared_ptr<Transport::IChannel> channel,
        std::shared_ptr<Service::SessionContext> session,
        std::shared_ptr<std::vector<Core::TagValue>> allResults,
        int attempt) {

    // 终止: 停止 → 收尾; 越界/切换到下一设备 → 继续处理下一设备
        // (不得在换设备时 FinishBatch: 否则第 2+ 台设备永不被轮询)
    if (_stopping.load()) {
        FinishBatch(group, allResults);
        return;
    }
    if (group->generation != _generation.load()) {
        AbandonBatch(group);   // 旧代: 丢弃, 不复活陈旧标签值
        return;
    }
    if (curIdx >= group->mergedRequests.size() ||
        group->mergedRequests[curIdx].deviceId !=
            group->mergedRequests[deviceIdx].deviceId) {
        ProcessDevice(group, curIdx, allResults, 1);
        return;
    }

    const auto& merged = group->mergedRequests[curIdx];

    // 设备级请求超时 (2026-08-24 收敛为唯一配置点)
    int requestTimeoutMs = 3000;
    auto timeoutIt = _deviceTimeoutMap.find(merged.deviceId);
    if (timeoutIt != _deviceTimeoutMap.end()) {
        requestTimeoutMs = timeoutIt->second;
    }

    const Core::ResilienceConfig res = ResilienceFor(merged.deviceId);

    // Deadline 预算 (ADR-0004 §3): 本次有效超时 = min(requestTimeoutMs, 剩余预算)
    const int budgetMs = RemainingBudgetMs(group);
    if (budgetMs <= 0) {
        // 预算耗尽: 不再发起 IO → 记入熔断 + 标 Bad, 链内推进 (不级联下一周期)
        session->RecordFailure();
        // KI-04: 预算耗尽通常因连接级拖累 → Degraded
        session->SetLifecycle(Service::DeviceLifecycleState::Degraded);
        AppendBadValues(group, curIdx,
            Core::Error{Core::Error::Code::Timeout,
                "deadline exhausted", {}}, *allResults);
        PollBatchChain(group, deviceIdx, curIdx + 1, reader, protocol,
                       channel, session, allResults, 1);
        return;
    }
    const int effectiveTimeoutMs =
        requestTimeoutMs < budgetMs ? requestTimeoutMs : budgetMs;

    // 插入点标记: 重试前回滚本次部分结果
    const size_t mark = allResults->size();

    // 异步批量读取 — 不再拷 batchTags; 传共享标签表 + tagIndices
    reader->ReadBatch(merged, group->tags, protocol, *channel,
                      effectiveTimeoutMs,
        [this, group, deviceIdx, curIdx, reader, protocol, channel, session,
         allResults, mark, res, attempt](std::vector<Core::TagValue> batchResults) {

            if (_stopping.load()) {
                FinishBatch(group, allResults);
                return;
            }
            if (group->generation != _generation.load()) {
                AbandonBatch(group);   // 旧代: 丢弃, 不复活陈旧标签值
                return;
            }

            allResults->insert(allResults->end(),
                batchResults.begin(), batchResults.end());

            // 熔断记账 (ADR-0001): 批内含可重试故障 → 失败方向;
            // 仅不可重试 Bad (如 InvalidResponse — 设备可达但数据异常) 视为成功
            bool hasRetryable = false;
            for (const auto& tv : batchResults) {
                if (tv.quality == Core::QualityCode::Bad &&
                    Core::IsRetryable(tv.lastError.code)) {
                    hasRetryable = true;
                    break;
                }
            }

            if (!hasRetryable) {
                session->RecordSuccess();
                PollBatchChain(group, deviceIdx, curIdx + 1, reader, protocol,
                               channel, session, allResults, 1);
                return;
            }

            // 可重试故障: maxAttempts/预算允许 → 回滚后退避重试同一请求
            if (attempt < res.maxAttempts && RemainingBudgetMs(group) > 0) {
                allResults->resize(mark);
                const int delayMs = BackoffDelayMs(res, attempt);
                RetryAfterBackoff(group, delayMs,
                    [this, group, deviceIdx, curIdx, reader, protocol, channel,
                     session, allResults, attempt]() {
                        if (_stopping.load()) {
                            FinishBatch(group, allResults);
                            return;
                        }
                        if (group->generation != _generation.load()) {
                            AbandonBatch(group);
                            return;
                        }
                        PollBatchChain(group, deviceIdx, curIdx, reader,
                                       protocol, channel, session, allResults,
                                       attempt + 1);
                    });
                return;
            }

            // 耗尽: 保留最终 Bad 行, 记入熔断, 推进下一 MergedRequest
            session->RecordFailure();
            // KI-04: 重试预算耗尽 + 批内仍含可重试错误 → 连接层故障 → Degraded
            // (协议级/解析级错误在 hasRetryable 阶段已被视为成功方向, 不会进此分支)
            session->SetLifecycle(Service::DeviceLifecycleState::Degraded);
            PollBatchChain(group, deviceIdx, curIdx + 1, reader, protocol,
                           channel, session, allResults, 1);
        });
}

// ──────────────────── 完成 / 辅助 ────────────────────

void PollingEngine::AbandonBatch(std::shared_ptr<PollGroup> group) {
    // 旧代组已不在 _groups 中 (Start 已 clear), 此处仅释放背压;
    // 关键是**不**调用 _onResults — 避免陈旧标签值写回 LatestValueStore。
    if (group) group->busy = false;
}

void PollingEngine::FinishBatch(
        std::shared_ptr<PollGroup> group,
        std::shared_ptr<std::vector<Core::TagValue>> results) {

    // 更新统计
    for (const auto& tv : *results) {
        _stats.totalReads.fetch_add(1);
        // 逐标签成功/失败经 Prometheus counter 暴露 (不再重复维护 PollStats 计数)
        if (tv.quality == Core::QualityCode::Good) {
            Core::metrics::CounterInc(Core::metrics::kPollReadsTotal,
                                      Core::metrics::Device(tv.deviceId));
        } else {
            Core::metrics::CounterInc(Core::metrics::kPollReadFailuresTotal,
                                      Core::metrics::Device(tv.deviceId));
        }
    }

    // 结果分发
    if (_onResults && !results->empty()) {
        _onResults(*results);
    }

    // 解除背压, 调度下一轮
    group->busy = false;
    ScheduleNext(group);
}

void PollingEngine::AppendBadValues(std::shared_ptr<PollGroup> group,
                                    size_t deviceIdx,
                                    const Core::Error& error,
                                    std::vector<Core::TagValue>& results) {
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const std::vector<Core::TagDefinition>& allTags = *group->tags;
    for (size_t idx : group->mergedRequests[deviceIdx].tagIndices) {
        if (idx >= allTags.size()) continue;
        Core::TagValue tv;
        tv.tagName = allTags[idx].name;
        tv.deviceId = group->mergedRequests[deviceIdx].deviceId;
        tv.quality = Core::QualityCode::Bad;
        tv.lastError = error;
        tv.timestamp = now;
        results.push_back(tv);
    }
}

std::shared_ptr<const Core::ProtocolConfig>
PollingEngine::GetProtocolForDevice(const std::string& deviceId) {
    auto it = _deviceProtocolMap.find(deviceId);
    if (it == _deviceProtocolMap.end()) return nullptr;

    // 协议快照缓存: 每协议名仅首访 lookup+拷贝一次, 稳态轮询零拷贝。
    // (原实现每设备每轮询周期 lookup 按值返回 + make_shared 再深拷一次)
    auto cit = _protocolCache.find(it->second);
    if (cit != _protocolCache.end()) return cit->second;

    auto result = _gateway.GetProtocolLookup()(it->second);
    if (!result.has_value()) return nullptr;

    std::shared_ptr<const Core::ProtocolConfig> snapshot =
        std::make_shared<const Core::ProtocolConfig>(
            std::move(result.value()));
    _protocolCache[it->second] = snapshot;
    return snapshot;
}

// ──────────────────── 韧性 / 重试辅助 ────────────────────

Core::ResilienceConfig PollingEngine::ResilienceFor(
        const std::string& deviceId) const {
    auto it = _deviceResilienceMap.find(deviceId);
    if (it != _deviceResilienceMap.end()) return it->second;
    return Core::ResilienceConfig();
}

int PollingEngine::RemainingBudgetMs(
        std::shared_ptr<PollGroup> group) const {
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            group->batchDeadline - std::chrono::steady_clock::now()).count();
    if (ms <= 0) return 0;
    return ms > 2147483647LL ? 2147483647 : static_cast<int>(ms);
}

int PollingEngine::BackoffDelayMs(const Core::ResilienceConfig& res,
                                  int failedAttempt) {
    // 指数退避: min(backoffMaxMs, backoffBaseMs × 2^(failedAttempt-1))
    long long delay = res.backoffBaseMs;
    for (int i = 1; i < failedAttempt && delay < res.backoffMaxMs; ++i) {
        delay *= 2;
    }
    if (delay > static_cast<long long>(res.backoffMaxMs)) {
        delay = res.backoffMaxMs;
    }
    return static_cast<int>(delay);
}

void PollingEngine::RetryAfterBackoff(
        std::shared_ptr<PollGroup> group, int delayMs,
        const std::function<void()>& cont) {
    // 独立临时 timer — 不占用组调度 timer; shared_ptr 保活至回调触发
    auto t = std::make_shared<asio::steady_timer>(_io);
    t->expires_after(std::chrono::milliseconds(delayMs));
    t->async_wait([t, cont](const asio::error_code& ec) {
        if (!ec) cont();   // cancelled → 丢弃 (Stop 场景)
    });
}

void PollingEngine::FailDevice(
        std::shared_ptr<PollGroup> group,
        size_t deviceIdx,
        const Core::Error& error,
        std::shared_ptr<std::vector<Core::TagValue>> results) {
    // 同设备的 MergedRequest 在 mergedRequests 中按 deviceId 连续存放
    // (Start 内 byDevice 为 std::map 聚合), 整段标 Bad 后推进下一设备段
    const std::string devId = group->mergedRequests[deviceIdx].deviceId;
    size_t idx = deviceIdx;
    while (idx < group->mergedRequests.size() &&
           group->mergedRequests[idx].deviceId == devId) {
        AppendBadValues(group, idx, error, *results);
        ++idx;
    }
    ProcessDevice(group, idx, results, 1);
}

}} // namespace MyProt::Polling

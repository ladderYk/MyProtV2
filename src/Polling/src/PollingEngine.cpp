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
#include "MyProt/Core/Log.hpp"
#include "MyProt/Core/Expected.hpp"
#include <algorithm>
#include <cstdio>
#include <map>

namespace MyProt { namespace Polling {

PollingEngine::PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway)
    : _io(io)
    , _gateway(gateway)
    , _stats()
    , _running(false)
    , _stopping(false) {}

// ──────────────────── 生命周期 ────────────────────

void PollingEngine::Start(const std::vector<Core::TagDefinition>& tags,
                          const std::vector<Core::DeviceConfig>& devices,
                          ResultDispatch onResults,
                          const Core::Optional<Core::ResilienceConfig>&
                              globalResilience) {
    // 语义 = 从头重启, 不是空操作: 已运行时再次 Start 会先整体退役上一代。
    // (热重载走 ApplySnapshot 做设备级 diff, 不需要停整代; 这里保留全量语义
    //  给冷启动与遗留调用点, 避免早期 `if (_running) return;` 让重启静默失效)
    Stop();                               // 清理上一代 (全部组退役 + 取消定时器)
    ApplySnapshot(tags, devices, std::move(onResults), globalResilience);
}

void PollingEngine::ApplySnapshot(
        const std::vector<Core::TagDefinition>& tags,
        const std::vector<Core::DeviceConfig>& devices,
        ResultDispatch onResults,
        const Core::Optional<Core::ResilienceConfig>& globalResilience) {

    _stopping.store(false);
    _running.store(true);
    _onResults = std::move(onResults);
    _protocolCache.clear();     // 协议快照随代刷新 (模板/线序变更下一轮读取即生效)

    // 1. 设备映射表整表重建 — O(设备数), 与组复用无关
    //    (韧性解析: 设备级覆盖 > 全局 > 内置默认)
    // KI-04: enabled=false 的设备不参与轮询, 生命周期固定 Disabled (需热重载恢复)。
    _deviceProtocolMap.clear();
    _deviceTimeoutMap.clear();
    _deviceResilienceMap.clear();

    std::map<std::string, bool> deviceEnabled;
    std::map<std::string, const Core::DeviceConfig*> deviceById;
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
        deviceEnabled[dev.id] = dev.enabled;
        deviceById[dev.id] = &dev;
        if (!dev.enabled) {
            Core::metrics::GaugeSet(
                Core::metrics::kDeviceLifecycleState,
                Core::metrics::Device(dev.id),
                static_cast<double>(Service::DeviceLifecycleGaugeValue(
                    Service::DeviceLifecycleState::Disabled)));
            LOG_INFO("Lifecycle",
                     "设备 %s 配置停用 (enabled=false), 跳过轮询",
                     dev.id.c_str());
        }
    }

    // 2. 按设备聚合参与轮询的标签
        // 写标签 (direction=write) 与推送标签 (source=push) 不参与轮询 —
    //   前者只作为 /api/data/write 的定向目标, 后者由南向 ingest 适配器产出;
    //   在引擎入口过滤, TagGrouper 无需感知.
    std::vector<std::string> deviceOrder;
    std::map<std::string, std::vector<Core::TagDefinition>> perDeviceTags;
    for (const auto& tag : tags) {
        if (tag.direction == "write" || tag.source == "push") continue;
        std::map<std::string, bool>::const_iterator en =
            deviceEnabled.find(tag.deviceId);
        if (en != deviceEnabled.end() && !en->second) continue;  // KI-04: 停用设备
        if (perDeviceTags.find(tag.deviceId) == perDeviceTags.end()) {
            deviceOrder.push_back(tag.deviceId);
        }
        perDeviceTags[tag.deviceId].push_back(tag);
    }

    // 3. 上一代组按设备归档 — 设备级 diff 的比对面
    std::map<std::string, std::vector<std::shared_ptr<PollGroup>>> existing;
    for (size_t i = 0; i < _groups.size(); ++i) {
        existing[_groups[i]->deviceId].push_back(_groups[i]);
    }
    _groups.clear();

    std::unordered_map<std::string, std::string> nextSignatures;
    int64_t activeTags = 0;

    for (size_t i = 0; i < deviceOrder.size(); ++i) {
        const std::string& devId = deviceOrder[i];
        const std::vector<Core::TagDefinition>& devTags = perDeviceTags[devId];
        activeTags += static_cast<int64_t>(devTags.size());

        // 合并跨度来自协议级配置; 协议查找失败时回退 Core::kDefaultMaxSpanBytes.
        int maxSpan = Core::kDefaultMaxSpanBytes;
        std::shared_ptr<const Core::ProtocolConfig> proto =
            GetProtocolForDevice(devId);
        if (proto && proto->maxSpanBytes > 0) maxSpan = proto->maxSpanBytes;

        // 标签引用了未声明的设备时与旧实现保持一致: 仍建组 (运行期按
        // DeviceNotFound 标 Bad), 签名退化为设备 id.
        Core::DeviceConfig fallback;
        fallback.id = devId;
        std::map<std::string, const Core::DeviceConfig*>::const_iterator dit =
            deviceById.find(devId);
        const Core::DeviceConfig& devCfg =
            (dit != deviceById.end()) ? *dit->second : fallback;

        const std::string sig = DevicePollSignature(
            devCfg, devTags, ResilienceFor(devId), maxSpan);
        nextSignatures[devId] = sig;

        std::map<std::string, std::vector<std::shared_ptr<PollGroup>>>::iterator
            prev = existing.find(devId);
        std::unordered_map<std::string, std::string>::const_iterator oldSig =
            _deviceSignatures.find(devId);

        // 设备轮询语义未变 → 整组复用: timer / in-flight 链 / deadline 预算全部
        // 保持原样, 本次热重载对该设备零扰动 (不重连、不空窗、不重置熔断)。
        if (prev != existing.end() && oldSig != _deviceSignatures.end()
                && oldSig->second == sig) {
            for (size_t k = 0; k < prev->second.size(); ++k) {
                _groups.push_back(prev->second[k]);
            }
            existing.erase(prev);
            continue;
        }

        // 变更/新增设备: 旧组退役 (in-flight 回调按 alive 丢弃), 按新标签表重建
        if (prev != existing.end()) {
            for (size_t k = 0; k < prev->second.size(); ++k) {
                RetireGroup(prev->second[k]);
            }
            existing.erase(prev);
        }
        BuildGroupsForDevice(devId, devTags, maxSpan);
    }

    // 4. 消失的设备 (配置删除或无标签) — 退役其全部旧组
    for (std::map<std::string, std::vector<std::shared_ptr<PollGroup>>>::iterator it =
             existing.begin(); it != existing.end(); ++it) {
        for (size_t k = 0; k < it->second.size(); ++k) RetireGroup(it->second[k]);
    }

    _deviceSignatures.swap(nextSignatures);
    _stats.activeTags.store(activeTags);
}

void PollingEngine::BuildGroupsForDevice(
        const std::string& deviceId,
        const std::vector<Core::TagDefinition>& deviceTags,
        int maxSpan) {
    // 每设备一份标签表 (不是全量): tagIndices 只在本设备内有效, 未变设备的组
    // 因此可以跨代复用 — 其他设备增删标签不会移动它的索引。
    std::shared_ptr<const std::vector<Core::TagDefinition>> sharedTags =
        std::make_shared<const std::vector<Core::TagDefinition>>(deviceTags);

    const std::vector<Gateway::TagGroup> tagGroups =
        _gateway.GetTagGrouper().GroupByScanRate(*sharedTags);

    std::map<int, std::shared_ptr<PollGroup>> byRate;
    for (size_t i = 0; i < tagGroups.size(); ++i) {
        const Gateway::TagGroup& tg = tagGroups[i];
        std::shared_ptr<PollGroup>& pg = byRate[tg.scanRateMs];   // 缺省 nullptr
        if (!pg) {
            pg = std::make_shared<PollGroup>(_io);
            pg->deviceId = deviceId;
            pg->scanRateMs = tg.scanRateMs;
            pg->tags = sharedTags;
        }
        std::vector<Gateway::MergedRequest> merged =
            _gateway.GetTagGrouper().CoalesceAdjacent(tg, *sharedTags, maxSpan);
        pg->mergedRequests.insert(
            pg->mergedRequests.end(), merged.begin(), merged.end());
    }

    for (std::map<int, std::shared_ptr<PollGroup>>::iterator it = byRate.begin();
         it != byRate.end(); ++it) {
        _groups.push_back(it->second);
        _stats.groupsBuilt.fetch_add(1);   // 热重载 diff 保真度指标 (只计新建的组)
        ScheduleNext(it->second);
    }
}

void PollingEngine::Stop() {
    _stopping.store(true);
    _running.store(false);
    for (size_t i = 0; i < _groups.size(); ++i) RetireGroup(_groups[i]);
    _groups.clear();
    _deviceSignatures.clear();   // 下次 Start 必须整代重建
}

void PollingEngine::RetireGroup(const std::shared_ptr<PollGroup>& group) {
    if (!group) return;
    // 先置 retired 再取消定时器: 定时器回调若已在队列中, 进入后按 alive 直接返回
    group->alive.store(false);
    asio::error_code ec;
    group->timer.cancel(ec);
}

// ──────────────────── 设备签名 (热重载设备级 diff) ────────────────────

namespace {

// 签名片段拼接: 用 \x1f 分隔, 避免 "ab"+"c" 与 "a"+"bc" 拼出同一个串
void SigAppend(std::string& out, const std::string& v) {
    out += v;
    out.push_back('\x1f');
}

void SigAppend(std::string& out, long long v) {
    out += std::to_string(v);
    out.push_back('\x1f');
}

void SigAppendBool(std::string& out, bool v) {
    out.push_back(v ? '1' : '0');
    out.push_back('\x1f');
}

// 变量表是无序容器 — 签名必须先排序, 否则同内容不同桶序会误判为变更
void SigAppendVars(
        std::string& out,
        const std::unordered_map<std::string, std::uint32_t>& vars) {
    std::map<std::string, std::uint32_t> ordered(vars.begin(), vars.end());
    for (std::map<std::string, std::uint32_t>::const_iterator it = ordered.begin();
         it != ordered.end(); ++it) {
        out += it->first;
        out.push_back('=');
        out += std::to_string(it->second);
        out.push_back(',');
    }
    out.push_back('\x1f');
}

} // namespace

std::string PollingEngine::DevicePollSignature(
        const Core::DeviceConfig& device,
        const std::vector<Core::TagDefinition>& deviceTags,
        const Core::ResilienceConfig& effectiveResilience,
        int maxSpan) {
    std::string sig;
    sig.reserve(deviceTags.size() * 128);

    // 设备侧 — 只列引擎读取的字段; 连接参数 (host/port/...) 归 ChannelManager
    // 的 diff 管, 这里引入反而会让“仅改地址”把轮询组也重建。
    SigAppend(sig, device.id);
    SigAppend(sig, device.protocol);
    SigAppendBool(sig, device.enabled);
    SigAppend(sig, static_cast<long long>(device.requestTimeoutMs));
    SigAppend(sig, static_cast<long long>(effectiveResilience.maxAttempts));
    SigAppend(sig, static_cast<long long>(effectiveResilience.backoffBaseMs));
    SigAppend(sig, static_cast<long long>(effectiveResilience.backoffMaxMs));
    SigAppend(sig, static_cast<long long>(effectiveResilience.failureThreshold));
    SigAppend(sig, static_cast<long long>(effectiveResilience.cooldownMs));
    SigAppend(sig, static_cast<long long>(effectiveResilience.halfOpenProbes));
    SigAppend(sig, static_cast<long long>(effectiveResilience.disableAfterDegraded));
    SigAppend(sig, static_cast<long long>(maxSpan));

    // 标签侧 — label/unit 仅用于展示, 不参与签名 (改中文名不应触发重连)
    for (size_t i = 0; i < deviceTags.size(); ++i) {
        const Core::TagDefinition& t = deviceTags[i];
        SigAppend(sig, t.name);
        SigAppend(sig, t.deviceId);
        SigAppend(sig, t.operation);
        SigAppendVars(sig, t.variables);
        SigAppend(sig, static_cast<long long>(t.scanRateMs));
        SigAppend(sig, t.finalType);
        SigAppendBool(sig, t.byteOrder.has_value());
        if (t.byteOrder.has_value()) {
            SigAppend(sig, static_cast<long long>(t.byteOrder.value()));
        }
        SigAppendBool(sig, t.coalesce);
        SigAppend(sig, static_cast<long long>(t.bitOffset));
        SigAppend(sig, t.direction);
        SigAppend(sig, t.source);
        SigAppend(sig, t.writeVariable);
        SigAppend(sig, t.readBackTag);
        SigAppend(sig, t.writeOperation);
        SigAppend(sig, t.writeBytesOperation);
        SigAppendVars(sig, t.writeVariables);
        SigAppend(sig, static_cast<long long>(t.converters.size()));
        for (size_t c = 0; c < t.converters.size(); ++c) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.17g,%.17g",
                          t.converters[c].k, t.converters[c].b);
            sig += buf;
            sig.push_back('\x1f');
        }
    }
    return sig;
}

// ──────────────────── 定时器调度 ────────────────────

void PollingEngine::ScheduleNext(std::shared_ptr<PollGroup> group) {
    // 退役防护: Stop→Start 连续调用时 (_stopping 已复位), 已退役组的
    // FinishBatch 会走到这里 — alive=false 直接丢弃, 防旧标签集复活
    if (_stopping.load() || !group->alive.load()) return;
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
    if (!group->alive.load()) return;   // 已退役组

    // 背压: 上一批未完成 → 跳过
    if (group->busy) {
        ScheduleNext(group);   // 背压: 上一批未完成, 跳过本轮
        return;
    }

    // KI-04: 已被判定 Disabled 的设备不再进入采集链。
    // 若继续走 ProcessDevice, GetOrCreateChannel 每轮都直接返回 ConfigError,
    // 虽不产生物理连接, 但仍会按扫描周期产出 Bad 值、累计失败计数并刷日志;
    // 这里只保留定时器空转等待 (Disabled 为会话内终态, 恢复只能靠热重载重建组,
    // Start 会重建 PollGroup, 因此跳过不会漏掉恢复)。
    if (!group->mergedRequests.empty()) {
        const std::string& deviceId = group->mergedRequests.front().deviceId;
        std::shared_ptr<Service::SessionContext> session =
            _gateway.GetChannelManager().GetSession(deviceId);
        if (session &&
            session->GetLifecycle() == Service::DeviceLifecycleState::Disabled) {
            ScheduleNext(group);
            return;
        }
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
            if (!group->alive.load()) {
                AbandonBatch(group);   // 已退役: 丢弃, 不复活陈旧标签值
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
                            if (!group->alive.load()) {
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
    if (!group->alive.load()) {
        AbandonBatch(group);   // 已退役: 丢弃, 不复活陈旧标签值
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
            if (!group->alive.load()) {
                AbandonBatch(group);   // 已退役: 丢弃, 不复活陈旧标签值
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
                        if (!group->alive.load()) {
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
    // 已退役组 (alive=false) 的收尾: 此处仅释放背压;
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

    // 结果分发 — 停止中/已退役组不分发: 旧标签集的 in-flight 结果可能晚于热重载
    // 的 Retain/ApplySnapshot 到达, 会把已删除标签重新写回实时快照.
    // (全停看 _stopping; 局部重建看 alive — 两者并发场景不同)
    if (_onResults && !_stopping.load() && group->alive.load()
            && !results->empty()) {
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

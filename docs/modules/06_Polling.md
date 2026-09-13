# MyProtV2 — Polling 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Gateway 模块](./05_Gateway.md)
> **下一篇**: [WebApi 模块](./07_WebApi.md)

---

**依赖**: `Gateway` + `Core`

## 6.1 PollingEngine

```cpp
// src/Polling/include/MyProt/Polling/PollingEngine.hpp

namespace MyProt { namespace Polling {

/// 结果分发回调: 每轮读取完成后调用, 参数为本轮所有 TagValue (含 Bad 质量)
using ResultDispatch = std::function<void(const std::vector<Core::TagValue>&)>;

/// 轮询引擎 — 按 scanRateMs 定时调度各设备的标签读取
/// 依赖: Gateway::ProtocolGateway (TagGrouper + TagReader + ChannelManager)
/// 调度: 每组一个 asio::steady_timer, 回调链驱动异步读取
/// 背压: 上一批未完成 → 跳过新批次
/// Deadline: 每轮读取预算 = scanRateMs, 超时标 Bad 不级联
class PollingEngine {
public:
    PollingEngine(asio::io_context& io, Gateway::ProtocolGateway& gateway);

    /// 启动轮询 (拷贝存储标签, 按 (scanRateMs, deviceId) 分组)
    /// @param globalResilience 全局韧性; 设备级 resilience 覆盖之, 均缺省用内置默认
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// 停止轮询 (取消各组定时器; 在飞批次自然完成, 靠代际号丢弃)
    void Stop();

    /// 统计 (字段定义见 PollStats.hpp)
    const PollStats& GetStats() const;
    bool IsRunning() const;

private:
    /// 轮询组 — 同一 scanRateMs 的标签 + 合并请求 (设备级独立预算, ADR-0004 R2)
    struct PollGroup {
        int scanRateMs;
        std::uint64_t generation;   // 装配代际 — 热重载后旧代 in-flight 组不得续排
        std::vector<Core::TagDefinition> allTags;   // 原始标签数组 (tagIndices 引用此数组)
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;                                    // 背压标志
        std::chrono::steady_clock::time_point batchDeadline;   // 本轮预算截止
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    std::atomic<std::uint64_t> _generation;   // 当前装配代际 (Start 时递增)
    ResultDispatch _onResults;
    std::unordered_map<std::string, std::string> _deviceProtocolMap;
    std::unordered_map<std::string, int> _deviceTimeoutMap;
    std::unordered_map<std::string, Core::ResilienceConfig> _deviceResilienceMap;
    std::vector<std::shared_ptr<PollGroup>> _groups;

    // 私有: ScheduleNext / OnTimerFire / ProcessDevice / PollBatchChain /
    //       FinishBatch / AbandonBatch / AppendBadValues / FailDevice /
    //       GetProtocolForDevice / ResilienceFor / RemainingBudgetMs /
    //       BackoffDelayMs / RetryAfterBackoff
};

}} // namespace MyProt::Polling
```

### 生命周期与调度

- **`Start`**：`_running=true`、`_generation++`、清空并重建 `_groups`。按 `(scanRateMs, deviceId)` 建组（设备级独立 timer + 独立 deadline 预算，ADR-0004 R2）；每组一个 `steady_timer` 并立即 `ScheduleNext`。`direction="write"` 的标签**不参与轮询**。
- **`Stop`**：`_stopping=true`、`_running=false`、取消各组定时器并清空 `_groups`。
- **`OnTimerFire`**：`_stopping` / 代际不符 / `ec`（已取消）直接返回；`group->busy` 为真则**跳过本轮**（背压），否则置 `busy=true`、设 `batchDeadline = now + scanRateMs`，进入 `ProcessDevice`。

### 读取链与韧性

`ProcessDevice` → `ChannelManager::GetOrCreateChannel`（连接类可重试故障按韧性指数退避后重试）→ `PollBatchChain` 逐个 `MergedRequest` 调 `TagReader::ReadBatch`：

- 批内含可重试故障 → 回滚本批结果、退避后重试同一请求；否则推进下一请求；最后 `FinishBatch`。
- **生效韧性**：设备级 `resilience` > 全局 `resilience` > 内置默认（`ResilienceFor`）。
- **预算**：每次有效超时 = `min(requestTimeoutMs, 剩余预算)`（`RemainingBudgetMs`）；预算耗尽 → 记熔断 + 标 Bad，不级联下一周期。
- **熔断记账**：批内出现可重试故障 → `RecordFailure`；仅不可重试 Bad（如 `InvalidResponse`）→ `RecordSuccess`；连接级故障另迁移设备生命周期 `Degraded`。

### 结果分发与统计

`FinishBatch` 累加 `totalReads`、按质量打 Prometheus counter（`myprot_poll_reads_total` / `myprot_poll_read_failures_total`），调用 `_onResults`（`ResultDispatch`），解除背压并 `ScheduleNext`。

`PollStats`（`src/Polling/include/MyProt/Polling/PollStats.hpp`）仅保留两个 `std::atomic<int64_t>`：`totalReads` / `activeTags`（读统计由上述 `Core::Metrics` counter 承担）。

### 热重载代际防护

`Start` 递增 `_generation`，每个 `PollGroup` 记录装配时的代际。所有异步回调入口（GOC 完成、退避续跑、`ReadBatch` 完成、`PollBatchChain` 入口）先校验代际：

- **代际不匹配** → `AbandonBatch(group)`：**不**分发结果、**不**更新统计，仅释放背压 —— 防止已删除/改名标签的陈旧值写回实时快照。
- **`_stopping`** → `FinishBatch`（收尾）。
- `ScheduleNext` / `OnTimerFire` 另有自身代际检查，保证旧代组不再续排。

### 离线值恢复（ResultDispatch，`src/App/main.cpp`）

对 `CircuitOpen` 引起的 `Bad` 值，在写入 `LatestValueStore` 前转译为 `Uncertain` 并沿用上次值：

- 条件：`tv.quality == Bad && tv.lastError.code == CircuitOpen`
- 查 `latest.Lookup(tagName, last)`，若上次质量是 **`Good` 或 `Uncertain`** → 替换 `typedValue`、`valueChanged = false`、`quality = Uncertain`、清空 `lastError`
- 判定同时接受 `Good` 与 `Uncertain`；若仅接受 `Good`，熔断持续时**第二个轮询周期即退化为 `Bad`**

**效果**：设备离线期间 `/api/data/latest` 返回 `Uncertain` + 上次值（而非 `Bad` + 0），UI 不抖动；设备恢复后自动转 `Good`。

相关实现：
- [src/Polling/include/MyProt/Polling/LatestValueStore.hpp](../../src/Polling/include/MyProt/Polling/LatestValueStore.hpp) `Lookup(tagName, out)` 接口
- [src/App/main.cpp](../../src/App/main.cpp) `ResultDispatch` lambda

### LatestValueStore 热重载清空

`ApplyRuntimeSync` 热重载时对 `LatestValueStore` 调用 **`Clear()`**（该类提供的方法），丢弃旧标签集数据；随后由轮询按新标签集重新积累。

- **线程安全**：`LatestValueStore` 以 `std::mutex` 保护 `Update` / `Snapshot` / `Lookup` / `Count` / `Clear`（io 线程写、WebApi 线程读）。
- **旧代 in-flight 回调**由上述**代际防护**拦下，不会把陈旧值写回。
- SSE 推送闭包与 `/api/data/latest` 读同一 store 指针，热重载后立即看到空表，下一轮轮询填充。

相关代码：
- `src/App/RuntimeGlue.cpp`（`ApplyRuntimeSync`：`latest.Clear()`）
- `src/Polling/include/MyProt/Polling/LatestValueStore.hpp`
- `src/Polling/src/PollingEngine.cpp`（`_generation` / `AbandonBatch`）

## 6.2 DataDispatcher（已撤回，历史备查）

> **⚠ 本节已撤回 (2026-08-29)**。原 `DataDispatcher`（扇出 + 订阅 + 死区过滤，`src/Polling/{include/MyProt/Polling/DataDispatcher.hpp, src/DataDispatcher.cpp}`）已删除。
>
> **撤回原因**：
> 1. **扇出/订阅 (Subscribe/Dispatch)**：v1 无第三方订阅者 —— `WebApiServer` 直接读 `LatestValueStore`，不需要 dispatcher 中介。
> 2. **死区过滤 (deadband / reportMode)**：v1 唯一实装在 `DataDispatcher`；撤回后这两个字段从未生效，故**已连同字段本身一并删除**——保留字段等于承诺不存在的行为。上报过滤作为独立扩展登记在 [ROADMAP.md](../ROADMAP.md)，需先有发布层设计（最新值缓存须保持真值，过滤只作用于对外发布通道）。
> 3. **asio::experimental::channel**：用 ASIO 试验接口做异步队列引入额外复杂度；单 `io_context` 部署下同步回调即可。
>
> **替代方案**：
> - **透传**：`PollingEngine::Start(..., onResults)` 接受 `ResultDispatch` 回调；`WebApiServer` 经 `LatestValueStore` 直读。
> - **死区**：无对应实现。E2E 中原先的 `ExceedsDeadband` 样例测的是**测试文件内部定义的 lambda**（假覆盖，给不存在的功能发绿灯），已删除；未来重立发布层时需重新设计并补真实用例。
>
> 本节代码块保留**作历史参考**；新代码不应引用 `MyProt::Polling::DataDispatcher`。

```cpp
// src/Polling/include/MyProt/Polling/DataDispatcher.hpp  (2026-08-29 已删除 — 撤回备查)

namespace MyProt::Polling {

/// 数据分发器 — 死区过滤 + 异步推送
class DataDispatcher {
public:
    using DataCallback = std::function<void(const std::vector<Core::TagValue>&)>;

    DataDispatcher(asio::io_context& io);

    /// 注册数据消费者 (MQTT/TimescaleDB/WebSocket/UserCallback)
    void Subscribe(DataCallback callback);

    /// 分发标签值 (PollingEngine 调用)
    void Dispatch(std::vector<Core::TagValue> values);

    /// 获取队列深度
    size_t QueueDepth() const;

    /// 优雅关闭
    void Shutdown();

private:
    using TagValueBatch = std::vector<Core::TagValue>;
    asio::experimental::channel<void(std::error_code, TagValueBatch)> _channel;
    std::vector<DataCallback> _subscribers;
    std::unordered_map<std::string, Core::TagValue> _lastValues; // 死区缓存
    mutable std::mutex _mutex;

    void StartConsumer();
    bool IsDeadbandExceeded(const Core::TagValue& current,
                            const Core::TagValue& previous,
                            double deadband);
};

} // namespace MyProt::Polling
```

---

> **文档版本**: v3.1
> **上一篇**: [Gateway 模块](./05_Gateway.md)
> **下一篇**: [WebApi 模块](./07_WebApi.md)

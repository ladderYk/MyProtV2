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

    /// 启动轮询 = Stop + ApplySnapshot (从头重建整代; 首次装配与遗留调用点使用)
    /// @param globalResilience 全局韧性; 设备级 resilience 覆盖之, 均缺省用内置默认
    void Start(const std::vector<Core::TagDefinition>& tags,
               const std::vector<Core::DeviceConfig>& devices,
               ResultDispatch onResults = nullptr,
               const Core::Optional<Core::ResilienceConfig>& globalResilience =
                   Core::Optional<Core::ResilienceConfig>());

    /// 应用一代配置 (首次装配与热重载共用入口) — 设备级 diff:
    /// 只有轮询签名变化的设备才重建组, 未变设备保留 timer / 在飞链 / 预算
    void ApplySnapshot(const std::vector<Core::TagDefinition>& tags,
                       const std::vector<Core::DeviceConfig>& devices,
                       ResultDispatch onResults = nullptr,
                       const Core::Optional<Core::ResilienceConfig>& globalResilience =
                           Core::Optional<Core::ResilienceConfig>());

    /// 停止轮询 (取消各组定时器; 在飞批次自然完成, 靠 alive 标志丢弃)
    void Stop();

    /// 统计 (字段定义见 PollStats.hpp)
    const PollStats& GetStats() const;
    bool IsRunning() const;

private:
    /// 轮询组 — 同一设备的同一 scanRateMs 的标签 + 合并请求 (设备级独立预算, ADR-0004 R2)
    struct PollGroup {
        std::string deviceId;       // 所属设备 (设备级 diff 的键)
        int scanRateMs;
        /// 本设备标签表 (不是全局标签表): tagIndices 只在本设备内有效,
        /// 其他设备增删标签不会移动索引, 组才能跨代复用
        std::shared_ptr<const std::vector<Core::TagDefinition>> tags;   // 本设备标签数组 (tagIndices 引用此数组)
        std::vector<Gateway::MergedRequest> mergedRequests;
        asio::steady_timer timer;
        bool busy;                                    // 背压标志
        std::chrono::steady_clock::time_point batchDeadline;   // 本轮预算截止
        /// 退役标志 — 设备被重建/删除或 Stop 时置 false。全局代际号只能表达
        /// "整代重建", 无法表达只重建部分设备, 故改为每组自带生命周期
        std::atomic<bool> alive;
    };

    asio::io_context& _io;
    Gateway::ProtocolGateway& _gateway;
    PollStats _stats;
    std::atomic<bool> _running;
    std::atomic<bool> _stopping;
    /// deviceId -> 轮询签名 (设备轮询字段 + 有效韧性 + maxSpan + 该设备全部轮询标签,
    /// 排除 label/unit)。签名相等 → 该设备的组整组复用
    std::unordered_map<std::string, std::string> _deviceSignatures;
    ResultDispatch _onResults;
    std::unordered_map<std::string, std::string> _deviceProtocolMap;
    std::unordered_map<std::string, int> _deviceTimeoutMap;
    std::unordered_map<std::string, Core::ResilienceConfig> _deviceResilienceMap;
    std::vector<std::shared_ptr<PollGroup>> _groups;

    // 私有: ScheduleNext / OnTimerFire / ProcessDevice / PollBatchChain /
    //       FinishBatch / AbandonBatch / RetireGroup / BuildGroupsForDevice /
    //       DevicePollSignature / AppendBadValues / FailDevice /
    //       GetProtocolForDevice / ResilienceFor / RemainingBudgetMs /
    //       BackoffDelayMs / RetryAfterBackoff
};

}} // namespace MyProt::Polling
```

### 生命周期与调度

- **`ApplySnapshot`**（首次装配与热重载共用入口，四步）：① 设备映射表整表重建；② 按设备聚合轮询标签（过滤 `direction="write"` / `source="push"` / `enabled=false` 的设备）；③ 上一代组按 `deviceId` 归档，重新计算 `DevicePollSignature` —— 签名相同则**整组复用**（timer、在飞链、deadline 预算全部保持原样），否则 `RetireGroup` 后按新标签表重建；④ 消失设备的组全部退役。末尾 `swap(_deviceSignatures)` 并更新 `activeTags`。
- **设备级 diff 的效果**：只改一个设备的地址/标签时，其他设备的轮询组零扰动 —— 不重连、不产生瞬时 Bad 窗口、不重置熔断与 deadline。`PollStats::groupsBuilt` 累加每次新建的组数，可直接观察 diff 保真度（一次保存后它只应增长被改设备的组数）。
- **`Start`** = `Stop` + `ApplySnapshot`：从头重建整代，供冷启动与遗留调用点使用（已运行时再次 `Start` 不是空操作）。
- **停用设备（KI-04）**：`device.enabled=false` 不建组、其标签（非 `write`/非 `push`）不进入 `pollTags`，启动时将 `myprot_device_lifecycle_state` 置于 `Disabled` 并打 INFO 日志；热重载改回 `true` 后随下一轮 `ApplySnapshot` 正常建组。运行时因 `Degraded` 累积进入 `Disabled` 的设备同样退出采集链：`OnTimerFire` 检测到 `GetLifecycle()==Disabled` 仅续排定时器，不再调 `GetOrCreateChannel`、不产出 Bad、不累计失败计数（触发禁用那一轮仍正常输出一次 Bad）；`Disabled` 为会话内终态，恢复只能靠热重载（`ChannelManager::ApplyDevices` 重建会话，见 [modules/05](./05_Gateway.md)）。
- **`Stop`**：`_stopping=true`、`_running=false`、逐组 `RetireGroup`、清空 `_groups` 与 `_deviceSignatures`（下次必须整代重建）。
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

### 变化检测

`LatestValueStore::Update` 是 `TagValue::valueChanged` 的**唯一计算落点**：写入时相对上一次写入判断，轮询与南向推送共用同一口径。

```
changed = 首次写入（无基线）
       || quality  != 上次 quality
       || !TypedValueEquals(typedValue, 上次 typedValue)   // Core/Value.hpp
```

- **不参与比较**：`timestamp` / `requestId` / `rawData` / `lastError`——否则每次采样都会被误判为变化
- **质量优先**：值完全相同但质量码变了（如 `Good→Uncertain`）也算变化，上游能看到降级事件
- **NaN 恒变**：`Float/Double` 按 `==` 比较，NaN 与自身不等，每次 NaN 采样都置位（可作设备异常信号）
- **生产者不置位**：`PollingEngine` / `TagReader` / `TagValueSink` 构造的 `TagValue` 中该字段无意义，消费方必须读 store 写回后的副本（`Lookup` / `Snapshot`）

下游三处同源消费：`/api/data/latest` 的 `changed`、SSE `snapshot` 的 `changed`、MQTT `publishMode=changed` 的过滤（后者另用 `BuildSignature` 做同口径签名，见 [modules/11](./11_Northbound.md)）。

### 热重载：设备级 diff 与退役防护

`ApplySnapshot` 为每个设备计算一份 **`DevicePollSignature`**：设备侧只列引擎实际读取的字段（`id/protocol/enabled/requestTimeoutMs`、7 个有效韧性字段、`maxSpan`），标签侧排除仅用于展示的 `label`/`unit`，`variables` 先排序再入签名（无序容器）。连接参数（host/port/凭据）**不进**轮询签名 —— 它们归 `ChannelManager` 的 diff 管，否则“仅改地址”会连带把轮询组也重建。

- **签名相等** → 该设备上一代全部组原样复用（timer 不断、在飞链继续、deadline 预算不重置）。
- **签名不等 / 新增设备** → 旧组 `RetireGroup`（`alive=false` 后再 `cancel`）并按新标签表重建；仍可达的已退役组回调一律 `AbandonBatch`。
- **设备消失** → 仅退役，不重建。
- **为什么不用单一全局代际号**：代际号只能表达“整代重建”，无法表达“只重建部分设备”。因此每个 `PollGroup` 自带 `alive`；所有异步回调入口（GOC 完成、退避续跑、`ReadBatch` 完成、`PollBatchChain` 入口）校验它：
  - **`alive=false`** → `AbandonBatch(group)`：**不**分发结果、**不**更新统计，仅释放背压 —— 防止已删除/改名标签的陈旧值写回实时快照。
  - **`_stopping`** → `FinishBatch`（收尾）。
- **每设备标签表**：`PollGroup::tags` 只含本设备标签，`tagIndices` 不会因其他设备增删标签而偏移 —— 这是未变组可以跨代复用的前提。
- **维护约束**：`TagDefinition` / `DeviceConfig` 新增任何影响轮询的字段时，必须同步追加到 `DevicePollSignature`，否则热重载会静默保留陈旧组（头文件已标注）。

### 离线值恢复（ResultDispatch，`src/App/main.cpp`）

对 `CircuitOpen` 引起的 `Bad` 值，在写入 `LatestValueStore` 前转译为 `Uncertain` 并沿用上次值：

- 条件：`tv.quality == Bad && tv.lastError.code == CircuitOpen`
- 查 `latest.Lookup(tagName, last)`，若上次质量是 **`Good` 或 `Uncertain`** → 替换 `typedValue`、`quality = Uncertain`、清空 `lastError`（`valueChanged` 由 `LatestValueStore::Update` 按「值不变、质量 Good→Uncertain」判为 true）
- 判定同时接受 `Good` 与 `Uncertain`；若仅接受 `Good`，熔断持续时**第二个轮询周期即退化为 `Bad`**

**效果**：设备离线期间 `/api/data/latest` 返回 `Uncertain` + 上次值（而非 `Bad` + 0），UI 不抖动；设备恢复后自动转 `Good`。

相关实现：
- [src/Polling/include/MyProt/Polling/LatestValueStore.hpp](../../src/Polling/include/MyProt/Polling/LatestValueStore.hpp) `Lookup(tagName, out)` 接口
- [src/App/main.cpp](../../src/App/main.cpp) `ResultDispatch` lambda

### LatestValueStore 热重载收敛

`ApplyRuntimeSync` 热重载时不再整表清空，而是在 io 线程上按顺序：`ApplyDevices`（通道/会话设备级 diff）→ `Retain(keepTagNames)`（新标签集收敛）→ `ApplySnapshot`（轮询设备级 diff）。三步都在同一 io 线程串行，无竞态：

- **保留未变标签**：仍存在的标签保留上次最新值与变化检测基线。保存瞬间实时页不再整表变空；引擎恢复后的首帧也不会因「首次写入」把未变值全部误标为 `changed`。
- **剔除已删除标签**：不在新标签集中的条目立即移除，`/api/data/latest` 与 SSE 不会展示旧标签集的残留值（改名等同于删除 + 新增）。
- **线程安全**：`LatestValueStore` 以 `std::mutex` 保护 `Update` / `Snapshot` / `Lookup` / `Count` / `Retain` / `Clear`（io 线程写、WebApi 线程读）。
- **旧代 in-flight 回调**：`_stopping` 期间 `FinishBatch` 只收尾、不分发结果；已退役组由上述 `alive` 守卫丢弃，不会把陈旧值写回。
- SSE 推送闭包与 `/api/data/latest` 读同一 store 指针：重载窗口内未变标签的值保持可见，下一轮轮询继续刷新。

相关代码：
- `src/App/RuntimeGlue.cpp`（`ApplyRuntimeSync`：`ApplyDevices` → `latest.Retain(keepTags)` → `ApplySnapshot`）
- `src/Polling/include/MyProt/Polling/LatestValueStore.hpp`
- `src/Polling/src/PollingEngine.cpp`（`ApplySnapshot` / `RetireGroup` / `alive` / `AbandonBatch`）

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

# MyProtV2 — 线程模型与数据流

> **所属**: MyProtV2 架构设计系列
> **上一节**: [分层架构与目录结构](./02_Layered_Architecture.md)
> **下一节**: [错误处理、优雅关闭与安全](./04_Error_Shutdown_Security.md)

---

## 五、线程模型

### 5.1 线程池划分

```
+--------------------------------------------------------------+
|                   io_context 线程池                          |
| +---------+ +---------+ +---------+ +---------+              |
| |Thread 0 | |Thread 1 | |Thread 2 | |Thread 3 |              |
| | asio::  | | asio::  | | asio::  | | asio::  |              |
| | run()   | | run()   | | run()   | | run()   |              |
| +----+----+ +----+----+ +----+----+ +----+----+              |
|      |          |          |          |                     |
|      +----------+----------+----------+                     |
|                         |                                   |
|          IoContextPool (round-robin 分配设备)               |
|          (目标架构; 初版: 单 io_context + 多线程 run())      |
+--------------------------------------------------------------+

+-------------------------+ +-------------------------------+
|   WebApi 线程           | |  ConsoleLogger (自研, 同步)   |
| (自研 HTTP/1.1 服务)    | |  LOG_* 门面写 stdout/文件     |
| 独立线程 + io.post 桥接 | |  v1 无独立日志线程            |
+-------------------------+ +-------------------------------+
```

### 5.2 线程职责

| 线程/池 | 数量 | 职责 | 阻塞约束 |
|---------|------|------|----------|
| io_context pool | `N = CPU 核数` | 所有 asio 异步 handler 链执行；协议解析、Socket I/O、轮询 timer 回调 | 零阻塞（仅 asio 异步操作；不挂起协程） |
| WebApi | 1 | HTTP 请求/响应（自研 HTTP/1.1 `WebApiServer`）；写路径经 `io.post` 投递 io_context | 独立于 io 线程池；回调中不阻塞 |
| ConsoleLogger（自研） | 0~1 | 写 stdout/文件；初版同步，v1 范围不引入 async logger | 同步无约束 |

### 5.3 并发安全策略

| 场景 | 策略 | 实现 |
|------|------|------|
| 同一设备请求串行 | `asio::strand` | 通道内 `_strand` 绑定 socket/端口操作；粒度为**物理端点**。**P1 D 撤回**：v1 单 io_context + 多线程 `run()` 部署下，per-bus strand 由 io_context 自身隐式串行化满足，通道内 `strand.post()` 为冗余间接层，v1 不再为 per-bus 单独建 strand；共享总线（RS-485）由 PollGroup 单生产者/TagGrouper 合并 + per-device 写互斥（P1 C）保证顺序，详见 ADR-0002 §6 撤回说明 |
| 不同设备请求并发 | 各请求独立入队 | v1 部署为单 io_context + 多线程 `run()`；不同设备请求天然在不同 worker 线程执行；P1 C per-device 写互斥仍按 `deviceId` 分桶保证同设备写不并发 |
| `ChannelManager::_channels` 读写 | 读多写少 | `std::mutex` + double-check locking（替代 C++17 `std::shared_mutex`） |
| ~~`DataDispatcher` 队列~~ | (已撤回) | A 撤回后 PollingEngine 通过 `ResultDispatch` 回调直接派发至消费者，跨线程同步在消费者侧完成 |
| `PollingEngine::_stats` | 多写多读 | 全 `std::atomic` |
| `RetryPolicy` 运行时更新 | 读多写少 | `std::mutex` (持有者各自保护；替代 `std::shared_mutex`) |
| WebApi → 引擎交互 | `io.post` 投递到 io_context | WebApi 工作线程只做投递（AppContext 聚合 + HandleWriteApi），不跨线程直接触碰引擎状态 |
| TagReader → SessionContext | 通过 `ChannelManager::GetSession(deviceId)` | SessionContext 存于独立 `_sessions` map, `std::mutex` 保护 |

> **吞吐策略**（[ADR-0001](../adr/0001-device-concurrency-vs-throughput.md)）：设备内严格串行不做并发放宽，有效吞吐经由 TagGrouper/MergeRequest 请求合并提升；`maxConcurrentRequests` 在 v1 固定为 1。

---

## 六、数据流全景

```
 +----------------+   +----------------------+
 |  tags.json     |   | protocols/*.json     |
 |  server.json   |   |                      |
 +-------+--------+   +----------+-----------+
         |                       |
         +-----------------------+
                   |
     +-----------▼----------------+
     |  ConfigDirectoryLoader    |  (JSON → ConfigRoot + Protocols + ServerConfig)
     |           |               |
     |  ConfigValidator          |  (引用完整 / 字段合法性 / 语义校验, 一体 Fail-Fast)
     +-----------+---------------+
                 |
     +-----------▼----------------+
     |  ProtocolGateway           |  (门面)
     |  +---------------------+   |
     |  | ChannelManager      |---+-- 连接 + 自动重连 + 熔断
     |  | TagReader           |---+-- 单读 + 批量读 + 单写
     |  | TagGrouper          |---+-- 标签→请求分组映射
     |  +---------------------+   |
     +-----------+---------------+
                 |
   +-------------+---------------+
   |             |               |
+--▼-------+  +--▼-----+      +---▼---------+
|PollingEng|  |WebApi  |      |SimulationSrv|
|(周期轮询) |  |/api/*  |      |(配置驱动仿真)|
|           |  |        |      |             |
+---+-------+  +--------+      +-------------+
    |
+---▼------------------+
|  TagReader (per Group)|
|  MergeRequest → Build|
|  → Channel.SendRecv  |
|  → ResponseParser    |
+---+------------------+
    | TagValue[]
    ▼
 +--+-----+      +-----------+      +-------------+
 |LatestV |      |WebApi     |      |WebApi SSE  |
 |alueStr |      |/data/     |      |/data/stream |
 |(Good/  |      |latest     |      |(实时推送)   |
 | Bad)   |      |(快照)    |      |             |
 +--------+      +-----------+      +-------------+
```

> 无 `DataDispatcher` 独立组件（回调直送消费者），无上报过滤：`TagDefinition.deadband` / `reportMode` 字段**已删除**；`valueChanged` 当前**未实现**（生产读路径从不置位，恒为 false，每轮透传）。

---

> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
> **上一节**: [分层架构与目录结构](./02_Layered_Architecture.md)
> **下一节**: [错误处理、优雅关闭与安全](./04_Error_Shutdown_Security.md)

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
|          (目标架构; v1 实态: 单 io_context + 单线程 run())      |
+--------------------------------------------------------------+

+-------------------------+ +-------------------------------+
|   WebApi 线程           | |  ConsoleLogger (自研, 同步)   |
| (自研 HTTP/1.1 服务)    | |  LOG_* 门面写 stdout/文件     |
| 独立线程 + io.post 桥接 | |  v1 无独立日志线程            |
+-------------------------+ +-------------------------------+

+-------------------------+ +-------------------------------+
|  Northbound 发布线程    | |  Southbound 入站线程          |
| MqttPublisher 独占      | |  SouthboundManager 独占       |
| io_context (发布/心跳)  | | io_context (订阅/监听/看门狗) |
+-------------------------+ +-------------------------------+

+-------------------------+ +-------------------------------+
|  MQTT 观测线程          | |  仿真 TCP 服务线程            |
| MqttProbeHub 独占       | |  SimulationServer 独占        |
| io_context (订阅/心跳)  | | io_context (accept/serve)     |
+-------------------------+ +-------------------------------+
```

### 5.2 线程职责

| 线程/池 | 数量 | 职责 | 阻塞约束 |
|---------|------|------|----------|
| io_context（单实例） | 1 — 主线程 `io.run_for(200ms)` 循环 | 所有 asio 异步 handler 链执行；协议解析、Socket I/O、轮询 timer 回调 | 零阻塞（仅 asio 异步操作；不挂起协程） |
| WebApi | 1 | HTTP 请求/响应（自研 HTTP/1.1 `WebApiServer`）；写路径经 `io.post` 投递 io_context | 独立于 io 线程池；回调中不阻塞 |
| Northbound 发布 | 0~1 | `MqttPublisher` 独占 `io_context`：连接/发布/心跳/重连/快照定时器；`enabled=false` 时零线程 | 零阻塞（全异步） |
| Southbound 入站 | 0~1 | `SouthboundManager` 独占 `io_context`：MQTT 收流、TCP accept/read、500ms 静默看门狗；无 `source=push` 标签时零线程 | 零阻塞；`Stop` 由调用线程 `join` |
| MQTT 观测（`MqttProbeHub`） | 1 | 管理面观测会话独占 `io_context`：代浏览器连接 broker、按会话订阅收流、事件环形缓存、状态心跳与空闲会话回收；进程启动即创建，与北向发布互不复用连接 | 零阻塞（全异步）；析构先 `StopAll` + `io.stop()` 再 `join` |
| 仿真 TCP 服务（`SimulationServer`） | 0~1 | 仿真器独占 `io_context`：accept、按协议回包、写回读值；未启用 `simulation` 时零线程 | 零阻塞；`Stop` 后 `join` |
| ConsoleLogger（自研） | 0~1 | 写 stdout/文件；初版同步，v1 范围不引入 async logger | 同步无约束 |

### 5.3 并发安全策略

| 场景 | 策略 | 实现 |
|------|------|------|
| 同一设备请求串行 | `asio::strand` | 通道内 `_strand` 绑定 socket/端口操作；粒度为**物理端点**。**P1 D 撤回**：v1 单 io_context + 单线程 `run()` 部署下，同通道 handler 天然按入队顺序执行，通道内 `strand.post()` 为冗余间接层，v1 不再为 per-bus 单独建 strand；共享总线（RS-485）由 PollGroup 单生产者/TagGrouper 合并 + per-device 写互斥（P1 C）保证顺序，详见 ADR-0002 §6 撤回说明 |
| 不同设备请求并发 | 各请求独立入队 | v1 实态为单 io_context + 单线程 `run()`（`main.cpp` 主循环 `io.run_for`）：全部 handler 在唯一 io 线程串行执行，并发指异步重叠 I/O 而非并行 handler；P1 C per-device 写互斥按 `deviceId` 分桶保证同设备写不并发，且不依赖线程模型 |
| `ChannelManager::_channels` 读写 | 读多写少 | `std::mutex` + double-check locking（替代 C++17 `std::shared_mutex`） |
| ~~`DataDispatcher` 队列~~ | (已撤回) | A 撤回后 PollingEngine 通过 `ResultDispatch` 回调直接派发至消费者，跨线程同步在消费者侧完成 |
| `PollingEngine::_stats` | 多写多读 | 全 `std::atomic` |
| `RetryPolicy` 运行时更新 | 读多写少 | `std::mutex` (持有者各自保护；替代 `std::shared_mutex`) |
| WebApi → 引擎交互 | `io.post` 投递到 io_context | WebApi 工作线程只做投递（AppContext 聚合 + HandleWriteApi），不跨线程直接触碰引擎状态 |
| TagReader → SessionContext | 通过 `ChannelManager::GetSession(deviceId)` | SessionContext 存于独立 `_sessions` map, `std::mutex` 保护 |
| Southbound 绑定表 / 看门狗 | 单线程独占 | 适配器回调、`Bindings`、`lastFrameMs` 只在自有 io 线程访问；`Apply`/`Stop` 先 `io.stop()` 再 `join`，之后才清理适配器与 sink 基线（[ADR-0019](../adr/0019-southbound-push-ingest.md)） |

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

> 无 `DataDispatcher` 独立组件（回调直送消费者），无上报过滤：`TagDefinition.deadband` / `reportMode` 字段**已删除**；`valueChanged` 只在推送入口（`TagValueSink`）按上次类型+值比较填充，生产轮询读路径从不置位（恒为 false）。
>
> **南向入站**（`SouthboundManager`）不经 `ProtocolGateway`：MQTT/TCP 适配器 → `PayloadMapper` → `TagValueSink` → 同一个 `LatestValueStore`。`source=push` 标签由入站适配器产出、`source=poll` 标签由轮询管线产出，两者共享快照但互不仲裁。

---

> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
> **上一节**: [分层架构与目录结构](./02_Layered_Architecture.md)
> **下一节**: [错误处理、优雅关闭与安全](./04_Error_Shutdown_Security.md)

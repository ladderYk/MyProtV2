# ADR-0023：南向 HTTP 轮询接入（网关作 HTTP 客户端）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-11-03 |
| 日期 | 2026-11-03 |
| 相关文档 | [Config_Schema §4.2](../Config_Schema.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0006](./0006-quality-semantics.md) · [ADR-0014](./0014-runtime-snapshot-single-source.md) · [ADR-0019](./0019-southbound-push-ingest.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md) |
| 提案来源 | [ROADMAP](../ROADMAP.md) #21：大量边缘网关 / 视觉设备 / 上位机只提供 REST 接口，网关上没有可用的二进制协议模板 |

---

## 背景

现有采集链路覆盖两类来源：

1. **请求-应答**：`PollingEngine` 按 `scanRateMs` 触发，经协议模板渲染字节帧、`IChannel::SendReceive` 收发；
2. **设备主动推送**：`SouthboundManager` + `IInboundTransport`（MQTT 订阅 / TCP 监听，ADR-0019）。

第三类设备只暴露 HTTP REST：网关必须**主动发起** HTTP 请求，但请求体是 JSON、响应是 JSON 数组，既不是字节级模板帧（无 `operations` / 成帧 / 字节序概念），也不是设备主动推送（网关是客户端）。硬塞进 `IChannel` 会把 HTTP 语义压进"字节流 + 单在途"的契约里，硬塞进 `ingest` 则会把"网关主动连接"伪装成"网关被动接收"。因此需要一个**应用层拉取适配器**，与 ADR-0019 的入站适配器并列。

同时必须裁决四件事：**配置形态**、**标签来源归属**、**写入口**、**失败与静默的质量语义**。

## 决策

### 2.1 配置：设备级 `pull` 块，与 `ingest` 并列

```json
"pull": {
  "type": "http",
  "http": {
    "url": "http://10.0.0.21:8080/api/values",
    "method": "GET",
    "intervalMs": 1000,
    "timeoutMs": 3000,
    "headers": { "Authorization": "Bearer <token>" },
    "body": "",
    "tagField": "tag",
    "valueField": "value",
    "tsField": "ts",
    "arrayOfItems": true,
    "maxSilenceMs": 0,
    "backoffBaseMs": 1000,
    "backoffMaxMs": 30000,
    "keepAlive": true
  }
}
```

- `pull` 与 `ingest` 均可选、互不排斥；**同一设备可以同时有 `ingest` 与 `pull`**（例如同一台 AGV 既上报状态又提供 REST 快照）。
- `type` 当前仅 `http`（保留联合类型，为后续 `snmp` / `opcua` 等留位）。
- `method` 仅 `GET` / `POST`；`POST` 时 `body` 作为请求体原样发送，未声明 `Content-Type` 时补 `application/json`。
- `intervalMs` ≥ 100（防止压垮设备），`timeoutMs` ≥ 100 且 **单轮串行**：本轮结束（成功/失败）后再等 `intervalMs` 发起下一轮，不堆积请求。
- `backoffBaseMs` / `backoffMaxMs`（默认 1000 / 30000）：**连续失败**时按 `base × 2^(n-1)` 指数退避并封顶 `backoffMaxMs`，且不短于 `intervalMs`；`backoffBaseMs=0` 关闭退避。成功一轮立即回到 `intervalMs`（见 §2.7）。
- `keepAlive`（默认 `true`）：复用同一 TCP 连接发起下一轮；服务端回 `Connection: close`、HTTP/1.0 未声明 keep-alive、或响应无 `Content-Length` 时自动重连（见 §2.7）。
- `headers` 是 `string -> string` 映射，用于静态凭据；**不进入日志**（仅打印 header 名集合）。
- `arrayOfItems`（默认 `true`）：响应为数组时按"每元素一条映射"解析；置 `false` 则沿用旧语义（整个数组作为一个值，通常配 `finalType=ByteArray`）。

### 2.2 标签来源：新增 `source="pull"`，三值互斥

`TagDefinition.source` 从 `poll | push` 扩展为 **`poll | push | pull`**：

| source | 产出方 | 是否参与轮询 | operation | 设备前置条件 |
|--------|--------|--------------|-----------|--------------|
| `poll`（默认） | `PollingEngine` | 是 | 必填 | — |
| `push` | `device.ingest`（MQTT/TCP） | 否 | 必须为空 | 设备有 `ingest` |
| `pull` | `device.pull`（HTTP） | 否 | 必须为空 | 设备有 `pull` |

- 选择三值而不是把 pull 并入 `push`：若并入，同一设备同时配 `ingest` 与 `pull` 时，一个 `push` 标签会被两个适配器争抢写入（双写、互相覆盖），违反 ADR-0019 的"单一来源、无仲裁"。
- `source=pull` 与 `direction=write` 互斥；`scanRateMs` 被忽略（校验器给 warning），节奏由 `pull.http.intervalMs` 决定。
- `finalType` 可省略（`Auto` 按 JSON 原生类型推断），也可显式指定做类型收敛。

### 2.3 适配器：`HttpPuller`，不复用 `IChannel`

`HttpPuller` 实现 ADR-0019 的 `IInboundTransport`（`Start(FrameHandler, ErrorHandler)` / `Stop()` / `IsRunning()`），从而：

- `SouthboundManager` 的绑定量、看门狗、`TagValueSink` 写入口、未声明标签告警等逻辑**零分叉**；
- 轮询引擎、`IChannel`、`Gateway`、`Engine` 不需要感知 HTTP。

线程模型：`HttpPuller` 自持一个**专属工作线程**执行阻塞式 HTTP/1.1 请求（`intervalMs` 节拍由该线程的 `condition_variable::wait_for` 控制，`Stop()` 立即唤醒并 `join`）；收到的帧通过 `io_context::post` **投递回 `SouthboundManager` 的 io 线程**再回调 `OnFrame`，保证 `binding->lastFrameMs` / 看门狗 / sink 仍单线程访问，无锁。

HTTP 客户端为**自实现最小客户端**（C++11 + standalone asio），原因：

- `third_party/cpp-httplib` 0.14.3 要求 C++14，与 v140/C++11 不兼容（ADR-0010），`WebApiServer` 已是自实现先例；
- 需求仅覆盖 `http://` 明文、`GET/POST`、`Content-Length` / `chunked` / 读到 EOF 定界，不需要 TLS/HTTP2/重定向。

**v1 明确不支持**：`https://`（TLS 在 ROADMAP 显式暂缓）、自动重定向、gzip 压缩；请求头固定发送 `Accept-Encoding: identity`，`Connection` 由 `keepAlive` 决定（默认 `keep-alive`，关闭时 `close`）。URL 仅接受 `http://host[:port][/path]`，端口缺省 80。

### 2.4 响应契约与映射

标准契约是 JSON 数组，每元素含标签名、值、可选时间戳：

```json
[
  { "tag": "Temp", "value": 23.5, "ts": 1730611200 },
  { "tag": "Run",  "value": true }
]
```

`PayloadMapper::MapJson` 增加 `arrayAsItems` 开关（默认 `false`，保持既有语义）：

- `arrayAsItems=true` 且 JSON 是数组：逐元素取 `tagField`（缺省回退 `defaultTag`，pull 路径为空故必须自带标签名）与 `valueField`，`tsField` 支持秒/毫秒自动判别（< 1e11 视为秒）；
- 元素不是对象、缺 `valueField`、缺标签名 → 整帧解析失败（不猜）；
- 非数组时行为不变：对象单条 / 对象批量 `{ "TagA": 1.5 }` 仍可用，便于非标准设备。

单帧上限 1MB（与 push 侧一致）。

### 2.5 质量语义：失败 `Bad`、静默 `Uncertain`

| 事件 | 质量 | 处理 |
|------|------|------|
| HTTP 2xx + 映射成功 | `Good` | 更新值/时间戳，刷新 `lastFrameMs` |
| DNS/连接/超时/非 2xx/非 JSON/映射失败 | `Bad` | 该设备**全部** pull 标签置 `Bad`（`lastError` 带原因），值置空（`Bad` = 无有效数据，见 ADR-0006），`rawData` 保留本次响应片段（非 2xx 最多 256 字节）；同一故障段内**不再叠加** `Uncertain` |
| 超过 `maxSilenceMs` 无有效帧（HTTP 成功但响应无命中标签） | `Uncertain` | 保留上次值，每段静默只报一次（复用 ADR-0019 看门狗） |

- 「整设备置 Bad」而不是"逐标签"：一次 HTTP 失败无法归因到单个标签，把未失败的标签留成 `Good` 会掩盖"值已过期"。
- 非 2xx 响应体只保留前 256 字节用于诊断，避免日志被 HTML 错误页淹没；连续失败日志按 key 限频 5s（复用 `WarnThrottled`）。
- `maxSilenceMs` 默认 0（关闭）：pull 的失败已由 `Bad` 表达，静默看门狗主要用于"HTTP 持续成功但目标标签长期缺失"的场景。
- 实现（2026-11-05 修正）：`OnError` / 解析失败会刷新 `lastErrorMs`，看门狗据此跳过“最近一次交互是明确失败”的绑定，避免同一故障段出现 `Bad → Uncertain → Bad` 抖动；静默基线 `lastFrameMs` 只在响应命中本设备标签时推进。回归断言：`FailedPullKeepsBadWithoutUncertainFlip` / `PullWithoutMatchingTagsTurnsUncertain`。

### 2.6 写入口与单一快照

与 ADR-0019 完全一致：`TagValueSink` 是唯一出口，写入 `LatestValueStore`。`/api/data`、北向 MQTT、SSE、写后校验都读同一份最新值，不区分来源。

### 2.7 连接复用与失败退避

**连接复用（`keepAlive`）**：`HttpExchange` 从“一轮一连接”改为**跨轮持有**的会话，复用同一个 `io_context` / socket / resolver：

- 仅当上一轮 **2xx + (`Content-Length` 或 `Transfer-Encoding: chunked`) + 服务端未要求关闭 + 无串包字节** 时保留连接；二者皆缺失（读到 EOF 定界）意味着连接已不可复用——chunked 与 `Content-Length` 同为自定界响应，可安全复用。
- 显式尊重 `Connection: close`；HTTP/1.0 响应只有带 `Connection: keep-alive` 才复用。
- **陈旧连接自愈**：复用的连接若已被对端静默回收，本轮在**收到任何响应字节之前**失败时，自动新建连接重试一次；已收到字节（半截响应）则不重试，按失败上报，避免把损坏的响应当成成功。
- `Stop()` 先 `post` 关闭动作打断在途请求（不必等满 `timeoutMs`），`join` 后再兜底关闭 socket；停机打断的那一轮**不上报** `Bad`，避免重载/退出瞬间污染质量。

**失败退避**：`ComputeRetryDelayMs(failures, intervalMs, baseMs, maxMs)` 为纯函数：`failures==0 → intervalMs`；否则 `min(maxMs, baseMs × 2^(failures-1))` 并下限钳到 `intervalMs`。设计取舍：

- 只在**连续失败**时退避，成功一轮立即复位——故障恢复后不会带着长间隔继续采；
- 退避不改变质量语义：失败仍由 `OnError → Bad` 表达，看门狗跳过“最近一次交互是失败”的绑定（§2.5）；
- 用确定性退避而非随机抖动：单设备场景可预期、可断言；多设备同时启动的惊群由“各设备各自计时 + 非同一时刻启动”缓解，若后续出现同刻重试风暴再引入按设备 ID 派生的抖动（复审条件 6）。

回归断言：`RetryDelayGrowsExponentially`（纯函数）、`ReusesConnectionWhenServerAllowsKeepAlive`（4 轮 1 连接）、`ReconnectsWhenServerAsksToClose`、`RetriesOnceOnStaleKeepAliveConnection`（陈旧连接重试且 0 错误）。

### 2.8 chunked 响应、拉取指标与生命周期联动（2026-11-06 增补）

**chunked 解码**：Python / Node / Java 的 Web 框架在未显式设置 `Content-Length` 时普遍回 `Transfer-Encoding: chunked`，初版对此直接报错，等于“接入即 `Bad`”，故补齐：

- `HttpExchange` 内新增 `ChunkSize → ChunkData → ChunkDataCrLf → ChunkTrailer` 四阶段状态机，随 `read_some` **增量消费**，不等待整包落地；
- 支持 chunk extension（`<hex>[;ext]\r\n`，`;` 之后忽略）与 chunked trailer（`0\r\n` 之后逐行读到空行为止，符合 RFC 7230 §4.1）；
- 解码后的 body 仍受 1MB 上限约束（与 push 侧一致）；原始收包缓冲放宽到 `2MB + 64KB`，避免“大量小分块”的元数据开销被误判为超限；
- chunk 长度行非法 / 数据块后缺 CRLF / 提前 EOF → 明确失败（日志含 `chunk` 关键字），不做容错猜测；
- chunked 属自定界响应，故纳入 §2.7 的 keep-alive 复用条件；tail 之后的剩余字节仍按“串包”处理（`_leftover` → 不复用）。

**指标**（`GET /metrics`，均带 `{device="<id>"}` 标签）：

| 指标 | 类型 | 语义 |
|------|------|------|
| `myprot_pull_rounds_total` | counter | 已发起的拉取轮次（成功 + 失败） |
| `myprot_pull_failures_total` | counter | 失败轮次（DNS / 连接 / 超时 / 非 2xx / 映射失败） |
| `myprot_pull_consecutive_failures` | gauge | 当前连续失败数，成功一轮归零 |
| `myprot_pull_backoff_ms` | gauge | 本轮结束后实际等待时长（成功 = `intervalMs`，失败 = 退避值） |
| `myprot_pull_last_success_timestamp_seconds` | gauge | 最近一次成功轮的 Unix 秒 |
| `myprot_pull_connections_total` | counter | 累计新建 TCP 连接数；与 `rounds_total` 的比值即复用率 |

`consecutive_failures` 与 `backoff_ms` 是排障主指标：前者配合 `failures_total` 区分“偶发单次失败”与“持续不可达”，后者可直接看出是否在退避、退到哪一档；`connections_total / rounds_total` 用于验证 keep-alive 是否真的生效（稳态应趋近 1 连接 / N 轮）。

**生命周期联动**：`HttpPuller` 每轮把结果经 `HealthHandler(deviceId, healthy)` 投递回 `SouthboundManager` 的 IO 线程；App 层在 `main.cpp` 绑定为 `SessionContext::SetLifecycle(healthy ? Connected : Degraded)`，复用 KI-04 语义（`Degraded` 累加 `degradedStreak`，达到 `disableAfterDegraded` 自动 `Disabled`）。

- **归属规则**：同一设备若还声明了轮询标签（`source=poll`），生命周期由轮询侧驱动，pull 侧不注册回调（`ownsLifecycle=false`），避免两路互相覆盖状态；
- **不重复累加**：`SetLifecycle` 幂等（同状态直接返回），同一故障段每轮上报 `Degraded` 只累加一次，不会因 `intervalMs` 短而加速触发 `Disabled`；
- **模块边界不变**：`Southbound` 不依赖 `Service`，只经 `std::function` 回调上抛，由 App 层完成绑定。

回归断言：`DecodesChunkedResponse` / `ChunkedResponseKeepsConnectionAlive`（4 轮 1 连接）/ `ChunkedTrailersAreAccepted` / `MalformedChunkSizeReportsError`、`ExposesPullMetrics` / `ExposesPullFailureMetrics`（含 `consecutive` 与 `backoff` 序列）、`ManagerReportsPullHealth`（200 → healthy，500 → unhealthy）。

## 备选方案与放弃理由

| 方案 | 放弃理由 |
|------|----------|
| 把 HTTP 包装成协议模板（`transport.type=http` + `operations`） | `IChannel` 契约是字节流 + 单在途请求-应答，HTTP 的头部解析、状态码、JSON 映射会被迫下沉进引擎；且一个设备多个标签会被拆成多次 HTTP 请求，无法利用"一次返回整包"的设备能力 |
| 复用 `ingest.type=http`（网关作服务端） | 方向相反：这类设备不会回调网关；语义伪装会让配置校验和运维诊断都误导 |
| 在 `PollingEngine` 内增加"HTTP 采集器"分支 | 轮询引擎的 scanRate / 合并 / 重试 / 写互斥全部围绕字节帧设计，HTTP 分支会让每条既有语义都出现例外 |
| pull 标签复用 `source=push` | 同设备 `ingest` + `pull` 并存时双写同一标签，来源无法判定 |
| 引入 cpp-httplib / libcurl | cpp-httplib 需 C++14（ADR-0010 不兼容）；libcurl 增加构建与部署依赖，而 v1 只需明文 GET/POST |
| 同步 HTTP 调用放在 `SouthboundManager` 的 io 线程 | 会阻塞看门狗与其他设备的回调；必须独立线程 + `post` 回投 |

## 后果

**正面**

- 只增加一个适配器与一个配置块，`IChannel` / `PollingEngine` / `Gateway` / `Engine` 零改动；
- 与 push 侧共享映射、写入口、看门狗、告警限频，质量语义一致；
- 适配器可独立测试：`MapJson(arrayAsItems)` 为纯函数，`HttpPuller` 可用本地 HTTP 服务做集成验证；
- 配置向后兼容：`pull` 与 `source=pull` 均可选，`schemaVersion` 仍为 2，旧配置行为不变；
- 兼容主流 Web 框架的默认 chunked 响应，且拉取侧的可观测性与生命周期表达与轮询侧对齐（指标 + `Degraded`）。

**代价 / 约束**

- 每设备一个 HTTP 轮询线程；设备数量大时线程数线性增长（后续可合并为共享调度器，见复审条件 2）；
- keep-alive 让单设备只占 1 条 TCP 连接，但长连接会被中间设备/NAT 空闲回收，故保留“陈旧连接重试一次”的兜底；
- 退避期间设备处于 `Bad`，恢复探测的最坏延迟是 `backoffMaxMs`（默认 30s），需要更快恢复可调小该值。
- 仅明文 HTTP，凭据以静态 header 形式存放于配置文件（依赖文件权限与网络隔离）；
- 不支持 gzip / 重定向；对端强制压缩时需设备侧关闭，或届时补齐解压；
- 一次响应失败会把该设备所有 pull 标签置 `Bad`，粒度较粗；
- 单轮串行 + `intervalMs` 意味着 `timeoutMs` 过长会拖慢实际采集周期（校验器在 `timeoutMs > intervalMs` 时给 warning）。

## 复审触发条件

1. 需要 HTTPS / mTLS → 复用/补齐 TLS 通道能力，并重新评估凭据存放与证书热更新；
2. pull 设备数量超过数十台 → 引入共享 io_context + 定时器调度，取消"一设备一线程"；
3. 需要 gzip / 重定向 → 扩展 HTTP 客户端而不是在 `HttpPuller` 内堆特例；
4. 需要按标签粒度归因失败（部分标签缺失只降部分质量）→ 需在契约中引入"缺失标签"表达，重新评估 2.5 的整设备置 Bad；
5. 需要 POST 动态请求体（模板化 body）→ body 需支持变量渲染，届时要与模板引擎的变量作用域对齐；
6. 多台 pull 设备在同一时刻集体重试（惊群）→ 在确定性退避上叠加按设备 ID 派生的抖动，并补充相应回归断言。

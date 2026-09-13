# MyProtV2 初始化时序

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [App 模块](./08_App.md)
> **下一篇**: [Simulation 模块](./10_Simulation.md)

---

> ⚠️ **本文档 v4.0 起按实态重写**。早期设计稿中的 `CliOptions` /
> `ApplicationBuilder` / `Application` 启动链路（`asynccohost`协程主链）**从未实现**；
> 其协程模型与 ADR-0010（C++11/VS2015，无 `asynccohost`/`co_await`）冲突，
> 相关章节已删除。实态为**回调式直接装配**：`main → RunProduction` 顺序
> 构造各模块对象并注入闭包，所有异步操作走 asio 尾参 handler + 单 `io_context`（单线程 `run_for`）串行化（P1 D per-bus strand 已撤回）。
> 见 [App 模块 §8.1](./08_App.md) 与 [RuntimeGlue.hpp](../../src/App/RuntimeGlue.hpp)。

---

## 9.1 完整启动时序（实态）

```
main()                                                  [main.cpp L293-309]
  ├─ 解析 CLI: --config <dir> / --port N; 缺省 configs:8080
  └─ RunProduction(configDir, apiPort)
       │
       ├─ 1. 环境准备                                  [main.cpp L84-86]
       │    ├─ SetConsoleOutputCP(65001)               // 控制台 UTF-8
       │    ├─ InstallCrashDiagnostics()               // SEH + dbghelp 栈回溯
       │    └─ SetupLoggingFromEnv()                   // MYPROT_LOG_LEVEL / MYPROT_LOG_FILE
       │
       ├─ 2. 配置加载（Fail-Fast，深度校验 + 解析一体）   [main.cpp L94-103]
       │    └─ Service::ConfigDirectoryLoader::Load(configDir, 1)
       │         ├─ 扫描 <dir>/protocols/*.json         [04_Service §4.2]
       │         ├─ 解析 tags.json
       │         ├─ 解析 server.json（缺失 = OK, listenPort=0 即关闭仿真）
       │         └─ ConfigValidator::Validate           // 校验与解析一体, 失败 exit 2
       │
       ├─ 3. 核心对象直构                              [main.cpp L111-199]
       │    ├─ asio::io_context io
       │    ├─ ProtocolLookup 闭包(protosPtr)          // 协议查找表
       │    ├─ ChannelFactory 闭包(devicesPtr, protosPtr) // v1 仅 Tcp
       │    ├─ ProtocolGateway gateway(io, lookup, factory)
       │    ├─ PollingEngine engine(io, gateway)
       │    ├─ LatestValueStore latest                  // /api/data/latest 快照源
       │    └─ ResultDispatch 闭包(latest)              // 轮询 → 快照 + 日志
       │
       ├─ 4. AppContext 聚合                            [main.cpp L211-215]
       │    └─ {&io, &gateway, &engine, startEngine,
       │        protosPtr, devicesPtr, tagsPtr,
       │        &latest, &sims, &simOwners, &protoStore, &store}
       │
       ├─ 5. 首次运行时装配                            [main.cpp L218]
       │    └─ ApplyRuntimeSync(configDir, ctx, &std::cout)
       │         ├─ 重建仿真器(若有 sim 设备)
       │         ├─ 清空实时快照
       │         └─ startEngine(tags, devices, resilience)
       │              └─ engine.Start(...)              // PollingEngine 装配
       │                                                    [Polling §6.2]
       │
       ├─ 6. 热重载联动注册                            [main.cpp L222-224]
       │    └─ store.SetReloadHandler([&] {
       │            return ApplyRuntimeSync(configDir, ctx, &std::cout);
       │       })
       │       (新配置非法 → ApplyRuntimeSync 报错 → ConfigStore 自动回滚)
       │                                                    [04_Service §4.3]
       │
       ├─ 7. 管理面装配                                 [main.cpp L226-265]
       │    ├─ WebApiServer server(apiCfg, store)
       │    ├─ extRoutes 注册: /api/data/write → /api/data → /api/sim
       │    ├─ SetStreamRoute("/api/data/stream", 3000ms) // SSE 实时推送
       │    └─ std::thread serverThread (server.Start)  // WebApi 独立线程
       │
       ├─ 8. 常驻运行                                   [main.cpp L271-280]
       │    └─ InstallStopSignals() → g_running=true
       │       while (IsRunning()) { io.run_for(200ms); }
       │
       └─ 9. 优雅关闭                                   [main.cpp L282-290]
            ├─ engine.Stop()                            // 取消所有 poll batch
            ├─ gateway.Shutdown()                       // 拒绝新连接
            ├─ server.Stop() + serverThread.join()
            └─ simOwners[*]->Stop()                     // 仿真器异步停
```

**关键语义**:

- **Fail-Fast**: 步骤 2 加载失败直接 `return 2`，绝不进入步骤 3
- **唯一装配入口**: `ApplyRuntimeSync` 首次与热重载同一条代码路径（步骤 5 与步骤 6 共享）
- **WebApi 独立线程**: 步骤 7 的 `serverThread` 跑 WebApiServer 的独立 `io_context`；主 io 循环与 WebApi 互不阻塞
- **退出码**: `0` 正常 / `2` 配置失败 / `3` WebApi 启动失败

---

## 9.2 传输层初始化（设备连接）

设备连接由 `PollingEngine::Start → ProcessDevice → ChannelManager::GetOrCreateChannel → ChannelFactory → IChannel::Connect` 链触发，按需懒连接，无独立"启动期连接所有设备"步骤。

```
PollingEngine::ProcessDevice(group, deviceIdx, ...)
  │
  └─ ChannelManager::GetOrCreateChannel(deviceId, handler)     [05_Gateway §5.2]
       ├─ 快路径: entry.channel 已连接 → 直接回调 ConnectResult{channel}
       ├─ entry.connecting → Busy 快速失败 (拒绝并发创建同一通道)
       ├─ 冷却期 (物理连接连续失败 2s 内) → ConnectionClosed 快速失败
       │
       ├─ 端点缓存命中 (_endpointCache[endpointKey]->IsConnected()) → 复用共享通道
       │
       └─ PerformConnect(entry, protocol, handler)
            ├─ ChannelFactory(io, deviceId)                     [main.cpp 闭包]
            │    ├─ 查 devicesPtr 找 device.protocol
            │    └─ 查 protosPtr 找 transport.type
            │         ├─ Tcp → TcpChannel(io, deviceId)          // v1
            │         └─ 其他 → 空 channel (连接失败走韧性重连)
            │
            ├─ channel->Connect(connection, timeout, handler)   [尾参 handler, ADR-0010]
            │    └─ asio 异步 DNS + TCP 连接 (看门狗超时 → Timeout)
            │
            ├─ 成功 → _endpointCache[endpointKey] = channel
            │
            ├─ PerformHandshake(channel, session, protocol.handshake, ...)
            │    └─ for each HandshakeStep:
            │         ├─ RenderHandshakeRequest(step.requestTemplate, 会话变量)
            │         ├─ channel->SendReceive(request, framingOverride, timeout, handler)
            │         ├─ ExpressionEvaluator(step.validCondition)
            │         └─ ExtractSessionVar("resp[A:B]") → SessionContext
            │
            └─ handler(ConnectResult{channel})   // 会话经 ChannelManager::GetSession(deviceId) 获取
```

**关键语义**:

- **按需懒连接**: 轮询到某 deviceId 才触发 Connect;无"启动期遍历连接所有设备"步骤
- **ChannelManager 内聚于网关**: 设备/通道/会话由 `ProtocolGateway` 持有的 `ChannelManager` 统一管理,见 [05_Gateway §5.2/§5.4](./05_Gateway.md)
- **连接失败走韧性重连**: [SessionContext 熔断器](../../src/Service/src/SessionContext.cpp) + `ResilienceConfig` (退避/阈值) 决定退避
- **句柄失效**: `gateway.Shutdown()` 关闭所有通道,`engine.Stop()` 取消所有 poll batch,新连接被拒绝

---

## 9.3 单次轮询周期时序

```
PollingEngine::OnTimerFire(group)                        [Polling §6.1]
  │
  ├─ 背压: group->busy → 跳过本轮; 否则 busy=true + batchDeadline=now+scanRateMs
  │
  └─ ProcessDevice(group, deviceIdx, results, attempt=1)
       │
       ├─ GetProtocolForDevice(deviceId) → ProtocolConfig
       ├─ ChannelManager::GetOrCreateChannel(deviceId, handler)   [05_Gateway §5.2]
       │    └─ 连接类可重试故障 → BackoffDelayMs 退避后重试 (ADR-0004)
       │
       └─ PollBatchChain(group, ..., curIdx, ...)                  [Polling §6.1]
            │
            ├─ TagReader::ReadBatch(merged, batchTags, protocol, channel, timeout, handler)
            │    ├─ 合并变量 (协议 static < op.inputs < tag.variables)
            │    ├─ InjectDerivedLengthVariables (op.outputs derivedLength)
            │    ├─ RequestBuilder::Build / BuildBytes → request 字节
            │    ├─ channel->SendReceive(request, framingConfig, timeout, handler)
            │    │    ├─ _inFlight 守卫 (单通道单请求; 第二笔立即 Busy)
            │    │    ├─ async_write(request)
            │    │    ├─ ReadChunk: async_read_some 累积, FrameComplete 判定成帧
            │    │    │    (LengthField / Fixed / Silence 三分支, 见 03_Transport §3.2)
            │    │    └─ watchdog 到期 → close + Timeout          [ADR-0004]
            │    │
            │    └─ ResponseParser::Parse(resp, op.responseParser, tag, byteOrder)
            │         ├─ CheckValidCondition (validCondition)
            │         ├─ dataStartIndex 定位 + tag.variables["ByteCount"] 定长
            │         └─ 按 tag.finalType + byteOrder 装配 TypedValue
            │
            ├─ 批内含可重试故障 → 回滚本批 + 退避重试同一请求
            └─ FinishBatch → _onResults(results)                    [main.cpp ResultDispatch]
                 ├─ latest.Update(results)                          // 实时快照
                 ├─ 质量统计 (Prometheus counter)
                 └─ 控制台打印 (Good/Bad/Uncertain)
```

**关键语义**:

- **回调式全链**: `SendReceive` / `ResponseParser::Parse`（内部按 `finalType` 装配 `TypedValue`）全部为尾参 handler 形式,无 `co_await` 挂起点
- **per-call framing**: 帧配置由 `SendReceive` 调用点传入(ADR-0002 单源决策,见 [03_Transport](./03_Transport.md))
- **串行化**: 全局单 `io_context` + 单线程 `run_for()`，回调天然串行；`TcpChannel` 另有 `_inFlight` 守卫保证单通道单请求（第二笔立即 Busy）。P1 D per-bus strand 已撤回，**不**走 `strand.post`（详见 [ADR-0002 §6 撤回说明](../adr/0002-transport-abstraction.md#6-撤回说明p1-d--2026-08-29)）
- **失败处理**: 单 tag 失败 → Bad Quality + 更新统计;整 batch 失败 → 标 group 失败,不影响其他 group

---

## 9.4 优雅关闭时序

```
SignalHandler (Ctrl+C / 关窗) → g_running=false
  │
  └─ 主线程 while (IsRunning()) 退出
       │
       ├─ 1. engine.Stop()                              // 停轮询, 取消各组定时器
       │    └─ 在飞批次自然完成, 代际号保证旧代不分发
       │
       ├─ 2. server.Stop()                              // 停管理面, 拒绝新请求
       │
       ├─ 3. 排空 io: while (!serverDone && now < deadline(5s)) io.run_for(20ms)
       │    └─ 执行在途写路径的 io.post, 否则 WebApi 线程会空等写超时 (最长 10s)
       │
       ├─ 4. serverThread.join()                         // WebApi 独立 io_context::stop() + join
       │
       ├─ 5. gateway.Shutdown()                          // 关闭所有 IChannel
       │
       └─ 6. simOwners[*]->Stop()                        // 每个 SimulationServer 自身 io_context::stop() + join
            └─ (避免 WinSock 跨线程 closesocket 假唤醒)
```

**关键语义**:

- **不依赖 io_context::stop()**: 关闭靠 `IsRunning()` 标志位 + 各组件显式 Stop,主循环自然退出
- **DRAIN 阶段**: 停管理面后继续 `io.run_for()` 排空在途写路径的 `io.post`，避免 WebApi 线程空等写超时；上限 5s 兜底
- **WebApi 独立线程**: 与主 io 循环解耦,关闭时先停其 io_context 再 join,避免死等
- **仿真器异步停**: `SimulationServer` 自身 io_context,`Stop()` 走 `io_context::stop()` 让 `run()` 返回后 join(避免 WinSock 跨线程 closesocket 假唤醒导致 join 挂起)

---

> **文档版本**: v4.1 (v4.0 基础上：流程图 1 行 ConvertToFinalType 工具函数撤回 (A2) → ParseResponse 内部 FromBytes 装配；"回调式全链"同步改写；"strand 串行化"行改"隐式串行化 P1 D 撤回" + ADR-0002 §6 引用)
> **上一篇**: [App 模块](./08_App.md)
> **下一篇**: [Simulation 模块](./10_Simulation.md)

# MyProtV2 — 错误处理、优雅关闭与安全

> **所属**: MyProtV2 架构设计系列
> **上一节**: [线程模型与数据流](./03_Threading_and_DataFlow.md)
> **下一节**: [可观测性、配置与构建](./05_Observability_Config_Build.md)

---

## 七、错误处理体系

### 7.1 错误码定义

> **唯一事实来源**：`Error` / `Error::Code` / `IsRetryable` 的 C++ 定义在 [`modules/01_Core.md`](../modules/01_Core.md)（§1.1 Expected / Error）。本节仅提供语义注解，**不重复定义**，以免双源漂移。

| 错误码 | 分类 | 语义与后果 | 可重试 |
|--------|------|-----------|:--:|
| `Timeout` | 传输 | 网络/设备超时 → 退避重试 | ✓（仅读） |
| `ConnectionRefused` | 传输 | 连接被拒 → 等待重连 | ✓（仅读） |
| `ConnectionClosed` | 传输 | 连接中断 → 自动重连 | ✓（仅读） |
| `Busy` | 传输 | 设备正忙 → 稍后重试 | ✓（仅读） |
| `InvalidResponse` | 解析 | 响应不符合 `validCondition` → Bad Quality | ✗ |
| `ParseError` | 解析 | 响应解析失败 → Bad Quality | ✗ |
| `BuildError` | 构建 | 请求构建失败 → Bad Quality，记录日志 | ✗ |
| `TypeConversionError` | 转换 | 原始字节 → `finalType` 失败 | ✗ |
| `WriteTimeout` | 写 | 写超时 — 不重试（写非幂等，避免重复写入） | ✗ |
| `WriteFailed` | 写 | 写失败 — 不重试 | ✗ |
| `ConfigError` | 配置 | JSON 解析/校验失败 → 启动 Fail-Fast | ✗ |
| `DeviceNotFound` | 配置 | 设备配置不存在 | ✗ |
| `TagNotFound` | 配置 | 标签未找到 | ✗ |
| `ProtocolNotFound` | 配置 | 协议名无解 | ✗ |
| `CircuitOpen` | 韧性 | 熔断器打开 → 拒绝请求（见 [ADR-0004](../adr/0004-timeout-retry-budget.md)） | ✗ |
| `InternalError` | 内部 | 不应发生的内部错误 | ✗ |
| `NotImplemented` | 内部 | 未实现功能（现仅 TlsChannel stub 使用；写路径已实现，见 ADR-0007 实现销账） | ✗ |

`IsRetryable(code)` 当且仅当 code ∈ {`Timeout`, `ConnectionRefused`, `ConnectionClosed`, `Busy`} 返回 true；**重试策略仅对读操作的这四类错误触发**，写操作的 `WriteTimeout`/`WriteFailed` 直接进入错误链路返回（重试的时间预算与熔断见 [ADR-0004](../adr/0004-timeout-retry-budget.md)）。

### 7.2 Monadic 链式处理

```cpp
// 典型单次采集：构建 → 发送 → 解析 → 类型转换 → 容错
//   ConvertToFinalType 的 byteOrder 默认 BigEndian; 生产代码的 TagReader
//   经 ProtocolLookup 取 ProtocolConfig 后按 ResolveByteOrder 裁决传入 (05_Gateway)
// 异步回调里 (C++11/VS2015): 协程 → 尾参 handler; 内部状态机显式建模
channel->SendReceive(request, frameCfg, timeoutMs,
    [=, &tag](Core::Expected<Core::Bytes> resp) {
        resp
            .and_then([&](const Core::Bytes& r) { return engine->ParseResponse(r, parser, session); })
            .and_then([&](const Core::Value& raw) { return ConvertToFinalType(raw, tag.finalType); })
            .map([&](const Core::Value& v) { return BuildTagValue(tag, v, QualityCode::Good); })
            .or_else([&](const Error& e) -> Expected<TagValue> {
                LOG_WARN("App", "[%s] Tag %s: %s", e.context.c_str(), tag.tagName.c_str(), e.message.c_str());
                return BuildTagValue(tag, {}, QualityCode::Bad, e);
            });
    });
```

### 7.3 异常策略

| 场景 | 处理方式 |
|------|----------|
| 构建/配置阶段 | `Expected<T>` 返回错误，由 main 处理退出 |
| 运行期 I/O 失败 | `Expected<T>` 返回，不抛 |
| asio 底层异常 | 异步 handler 看 `ec` 入参判定 → 包装为 `Expected::Error` |
| 断言失败 (Debug) | `assert()` 终止 |
| 不可恢复 (OOM 等) | `std::terminate` |

### 7.4 重试时间预算与熔断

> **完整模型与推导见 [ADR-0004](../adr/0004-timeout-retry-budget.md)；参数落地见 [Config_Schema §11](../Config_Schema.md)（`resilience` 块）。本节仅给出错误处理视角的要点。**

**超时分类**：`connection.timeoutMs` / `handshake[].timeoutMs` / `device.requestTimeoutMs` 都是**单次故障检测看门狗**（请求-应答看门狗，2026-08-24 起收敛为设备级 `requestTimeoutMs`，见 ADR-0004 R1）；在此之上引入 **deadline（总截止时间）** 作为整个逻辑读取（含重试/重连）的总预算。`device.requestTimeoutMs` 不是期望延迟——ADR-0001 的 P50/P99 仅统计"成功且无重试"的单物理请求，与看门狗正交。

**截止时间传播**：逻辑读取入口生成 deadline 并向下传播；第 k 次尝试有效超时 = `min(device.requestTimeoutMs, 剩余预算)`；剩余预算不足时放弃重试，返回最后一次错误并标记 Bad Quality。重连（含握手）消耗同一 deadline，不另开预算。轮询读取 deadline = `scanRateMs`（超预算不级联到下一周期），按需读取 deadline = `device.requestTimeoutMs × maxAttempts`（上限 10s）。

**重试/退避**：仅 `IsRetryable` 的四类读错误触发；`maxAttempts` 默认 3（读），1（写，写非幂等恒不重试）；退避 = `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + 抖动。

**熔断**：以**逻辑读取失败**（预算耗尽）为计数单位，连续 `failureThreshold`（默认 5）次 → 打开；`cooldownMs`（默认 10000）内请求直接返回 `CircuitOpen`，**不发实际 I/O**（共享总线第二道防饥饿闸门）；随后 `halfOpenProbes`（默认 1）次探测，成功闭合、失败重开。熔断状态机接线仍跟踪于 [ADR-0003](../adr/0003-known-issues.md) KI-04/KI-05。

### 7.5 质量码映射（Error → QualityCode）

> **唯一事实来源**：`QualityCode` 三值语义与 `Error::Code → QualityCode` 映射规则在 [`modules/01_Core.md`](../modules/01_Core.md) §1.3/§1.3.1；裁决见 [ADR-0006](../adr/0006-quality-semantics.md)。本节不重复定义。

要点：`Good` = 完整成功；`Uncertain` = 设备在线但数据降级（如 `InvalidResponse` / `TypeConversionError` 两个生产者，`rawData` 保留原始字节）；`Bad` = 无有效数据（其余一切错误）。判定原则一句话：**能对话但答非所问 → Uncertain；没法对话或请求没出门 → Bad**。所有模块的失败分支按映射规则置值，不得手写例外；传输类错误在重试窗口内不产出 TagValue，预算耗尽后按 `Bad` 产出（衔接 §7.4 / ADR-0004）。

---

## 八、优雅关闭流程

### 8.1 关闭阶段

```
Signal (SIGINT/SIGTERM)
  |
  v
Phase 1: STOP_POLLING    (≤ 2s)
  ├─ PollingEngine::Stop()
  ├─ 取消各组 steady_timer
  └─ 在飞批次自然完成 (代际号丢弃旧代)

Phase 2: DRAIN_REQUESTS  (≤ 5s)
  ├─ 等待所有 in-flight SendReceive 完成
  └─ ChannelManager 拒绝新的 GetOrCreateChannel

Phase 3: DISCONNECT      (≤ 3s)
  ├─ ChannelManager::ShutdownAll()
  ├─ 每个 channel.Disconnect()
  └─ 关闭所有 socket

Phase 4: STOP_WEBAPI     (≤ 2s)
  ├─ WebApiServer::Stop()
  └─ 停止自研 HTTP/1.1 监听线程
       └─ 期间主 io 继续 run_for 排空在途写回调, 再 join WebApi 线程

Phase 5: STOP_IO         (立即)
  ├─ gateway.Shutdown()
  └─ join / 停止仿真器工作线程
```

### 8.2 信号处理

```cpp
// src/App/SignalHandler.hpp
class SignalHandler {
public:
    SignalHandler(asio::io_context& io);
    // C++11: 通过 handler 回调交付信号编号; 取代 asio::awaitable<int>
    void WaitForSignal(std::function<void(int /*signo*/)> onSignal);
private:
    asio::signal_set _signals;
};

// src/App/main.cpp (v1 实态; 完整九步装配见 [App 模块设计](../modules/08_App.md) §8.1 与仓库源码)
//   无参数 = 默认启动网关 (configs 目录, API 8080); E2E 走独立 MyProt.E2E 进程
int RunProduction(const std::string& configDir, uint16_t apiPort) {
    // 1) ConfigDirectoryLoader::Load(configDir, 1)   解析+深度校验一体, 失败 exit 2
    // 2) PDULengthRegistry 注册协议插件 (Modbus FC15/FC16 长度策略)
    // 3) 构造闭包依赖 (ProtocolLookup / ChannelFactory / EngineStarter / ResultDispatch)
    // 4) 装配 ProtocolGateway + PollingEngine + LatestValueStore → AppContext 聚合
    // 5) ApplyRuntimeSync(configDir, ctx)             首次装配 (仿真器 + 引擎),
    //                                                 与热重载同一入口 (RuntimeGlue)
    // 6) WebApiServer 独立线程 + SetExtHandler(HttpRouter) + SetStreamRoute(SSE)
    // 7) 常驻: while (IsRunning()) { io.run_for(200ms); }   信号 → g_running=false
    // 8) engine.Stop() → gateway.Shutdown() → server.Stop() → join
    //    (即 §8.1 五阶段在单线程装配下的同步顺序收敛)
}
```

---

## 九、安全设计

### 9.1 传输层安全 (TLS)

```cpp
// TransportConfig 增加 TLS 变体
struct TlsTransportConfig {
    uint16_t defaultPort = 0;     // 0 = 由 DeviceConfig.port 决定
    std::string certFile;       // 客户端证书路径 (PEM)
    std::string keyFile;        // 私钥路径
    std::string caFile;         // CA 证书 (服务端验证)
    bool verifyServer = true;   // 是否验证服务端证书
};

// TcpTransportConfig 与 SerialTransportConfig 定义详见 [Core 模块设计](../modules/01_Core.md)
// tagged struct (取代 std::variant; ADR-0010 §3)
struct TransportConfig {
    enum class Type { Tcp, Tls, Serial } type;
    TcpTransportConfig    tcp;      // type==Tcp    生效
    TlsTransportConfig    tls;      // type==Tls    生效
    SerialTransportConfig serial;   // type==Serial 生效
};
```

`TlsChannel : public IChannel`，内部使用 `asio::ssl::stream<asio::ip::tcp::socket>`，其余接口与 `TcpChannel` 完全一致。

### 9.2 API 鉴权

`WebApiServer` 内置 Bearer Token 验证（`AuthMiddleware`：常量时间比较 + 敏感端点令牌桶限流）；token 取环境变量 `MYPROT_API_TOKEN`，不入配置文件：

token 校验须用**常量时间比较**（防时序侧信道）；token 优先取环境变量 `MYPROT_API_TOKEN`，日志脱敏。生产模式（`webApi.requireAuth = true`）下 token 缺失即启动 Fail-Fast。完整管理面安全（TLS、限流、绑定）见 §9.4 与 [ADR-0008](../adr/0008-management-plane-security.md)。

### 9.3 设备级认证

`DeviceConfig` 扩展为：

```cpp
struct DeviceConfig {
    // ... 基础字段
    Core::Optional<std::string> username;
    Core::Optional<std::string> password;  // TODO: 加密存储方案
};
```

认证在实际协议层面处理（如 S7 的 CPU 密码、OPC UA 的 UserTokenPolicy），不由网关框架统一处理。

### 9.4 管理面安全（WebApi）

> **完整裁决见 [ADR-0008](../adr/0008-management-plane-security.md)；端点与配置落地见 [modules/07_WebApi](../modules/07_WebApi.md)。** 管理面（北向）安全等级不应低于设备侧（南向）。

- **TLS**：管理面 TLS 为预留项（自研 HTTP/1.1 之上启用待后续评估；见 ADR-0008 与 [ROADMAP.md](../ROADMAP.md)）；`webApi.certFile`/`keyFile` 配置字段保留。
- **鉴权**：bearer token 常量时间比较；`requireAuth = true`（生产建议）— token 缺失则 Fail-Fast；token 取环境变量、日志脱敏。
- **限流**：状态变更类端点（`/api/config/reload`、`/api/data/write` 等）按客户端 IP 令牌桶限流（默认 5 rps / 突发 10），超限 `429`；只读与探针端点默认不限流。
- **绑定与安全头**：默认仅环回 `127.0.0.1`，远程管理需显式 `webApi.bindAddress = "0.0.0.0"` 并同时启用 TLS + token；响应附 `X-Content-Type-Options: nosniff`、`Cache-Control: no-store`。

---

> **文档版本**: v4.0（v3.1 基础上实态化：§8.2 启动示例由未实装的 RunApp/ApplicationBuilder 改为 RunProduction 八步概要；Phase 4 / §9.2 / §9.4 的 WebApiHost/httplib 措辞改 WebApiServer 自研 HTTP/1.1；§9.4 TLS 标注预留项）
> **上一节**: [线程模型与数据流](./03_Threading_and_DataFlow.md)
> **下一节**: [可观测性、配置与构建](./05_Observability_Config_Build.md)

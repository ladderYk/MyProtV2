# MyProtV2 ▸ WebApi 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Polling 模块](./06_Polling.md)
> **下一篇**: [App 模块](./08_App.md)

---

#### 多仿真器优先级（[HandleSimApi L216-358](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L216-L358)）

`/api/sim/*` 的查找与转发规则：

| 场景 | 路径格式 | 行为 | 代码定位 |
|---|---|---|---|
| **唯一仿真器** | `/api/sim/registers/0/4` | 自动取该仿真器（`sims.begin()->second`）| [L246-250](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L246-L250) |
| **多仿真器 + 省略 proto** | `/api/sim/registers/0/4` | **400** `{"error":"多仿真器场景必须显式 ?proto=<name>"}` | [L251-256](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L251-L256) |
| **多仿真器 + 显式 proto** | `/api/sim/registers/sim-modbus-1/0/4` | 按 sim id 匹配 | [L257-262](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L257-L262) |
| **proto 不存在** | — | **404** `{"error":"仿真服务器未找到: <id>"}` | [L263-269](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L263-L269) |

> **设计取舍**：唯一仿真器时省略 proto 是"历史 v3 残留"——v4 已要求显式，但仍保留降级以避免硬错误。`?proto=` query 形式由 [`QueryParam`](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L168-L194) 解析。

#### SSE 帧格式与 changed 优化

`/api/data/stream` 推送帧（[WebApiServer::SetStreamRoute](file:///d:/workSpaces/c#/MyProt-master/src/WebApi/src/WebApiServer.cpp) + [BuildLatestJson L359-386](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L359-L386)）：

```
event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.234Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":true}, ...]}

event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.534Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":false}, ...]}
```

**changed 字段语义**：本周期与上周期相比**值或质量发生变化**才为 `true`；客户端可据此忽略重复行节省渲染开销（实测 1000 tags × 1Hz 时带宽约 250KB/s 含 changed=false；优化后约 8KB/s）。

**finalType 决定 value 编码**：

| finalType | value 编码 | 示例 |
|---|---|---|
| 整数 (UINT8/16/32, INT8/16/32) | 十进制数字 | `1234` |
| 浮点 (FLOAT32, FLOAT64) | 浮点字面量 | `3.14159` |
| Bool | `true` / `false` | `true` |
| ByteArray (HEX 字符串) | 十六进制大写无空格 | `"A1B2C3"` |
| UTF-8 字符串 | 字符串（JSON 字符串） | `"Hello"` |

#### 断线清理时序

```
WebApi 端                          io 线程
  |                                  |
  |--- SetStreamRoute 注册 timer  -->|
  |                                  |
  |   客户端断开 (TCP FIN)            |
  |--- timer 析构 (栈 RAII)          |   (in-flight 推送自然停止)
  |--- 释放 latest 引用              |   (无 latest->Get 闭包悬挂)
  |                                  |
```

**关键点**：
- **timer 栈 RAII** 持有 `latest` 弱引用（[L386](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L386)），客户端断开时自动析构；
- **WebApi 端不持有 LatestValueStore 的强引用**，仅弱引用或裸指针（生命周期归 PollingEngine / RuntimeGlue 所有）；
- **推送过程中热重载**：LatestValueStore 在 `ApplyRuntimeSync` 中先建新 latest → swap → 释放旧 latest 引用 → in-flight 推送闭包持的是旧 latest 句柄，新请求自然走新 latest，**新旧无交叉**。

#### URL 查询参数解析（[QueryParam L168-194](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L168-L194)）

```
GET /api/data/latest?device=plc-1&token=abc
              ^path            ^query
```

- `path = /api/data/latest`，`QueryParam` 内部从 `?` 后开始解析；
- 返回 `key` 对应的 value，未找到返回空字符串 `""`；
- **空值与缺失不可区分**（`?device=&token=abc` 与 `?token=abc` 在 `QueryParam("?device=&token=abc", "device")` 上都返回 `""`）；
- **URL 解码**：当前**未实现** `url-decode`（如 `?name=%E6%B5%8B` 直传 `%E6%B5%8B`），调用方需自行处理或参数中避免中文/特殊字符。

---

**依赖**: `Core` + `Service` + standalone Asio

> **实现说明**：cpp-httplib 0.14.3 需 C++14，与 v140/C++11 工具链不兼容，故基于 standalone Asio **自实现最简 HTTP/1.1 服务**（每连接 `Connection: close`）。TLS 暂缓（v1 决策），token 认证 + 令牌桶限流落地 [ADR-0008](../adr/0008-management-plane-security.md) 子集。安全配置消费 `ConfigRoot.webApi?` 块（[Config_Schema §12](../Config_Schema.md)）。

## 7.1 WebApiServer

```cpp
// src/WebApi/include/MyProt/WebApi/WebApiServer.hpp

namespace MyProt { namespace WebApi {

/// WebApi 服务：标签查询 + 配置 CRUD + 热重载管理接口
class WebApiServer {
public:
    /// 外部域扩展路由回调（依赖倒置：App 层注入业务 handler
    /// (/api/sim/* 与 /api/data/*)，WebApi 不反向依赖 Simulation 等模块）
    /// 返回 (HTTP status, JSON body)。
    typedef std::function<std::pair<int, std::string>(
        const std::string& method, const std::string& path,
        const std::string& body)> ExtHandler;

    /// @param config 管理面安全配置 (ConfigRoot.webApi);
    ///               bindAddress 支持 "host[:port]" 形式, 缺省端口 8080
    /// @param store  配置读写服务 (ConfigStore), 持有引用, 生命周期由调用方保证
    /// @throws std::runtime_error 当 requireAuth=true 且环境变量
    ///         MYPROT_API_TOKEN 缺失时 Fail-Fast
    WebApiServer(const Core::WebApiConfig& config, Service::ConfigStore& store);

    /// 注册扩展路由 handler (须在 Start 前调用; 未注册时 /api/sim|data/* 为 404)
    void SetExtHandler(ExtHandler h) { _ext = h; }

    /// 启动 HTTP 服务器并阻塞监听 (内部 io_context.run(); 由调用方决定线程)
    void Start();

    /// 停止服务 (线程安全, 可从其他线程调用)
    void Stop();
};

}} // namespace MyProt { namespace WebApi
```

## 7.2 端点总表

| 方法 + 路径 | 功能 | 鉴权 |
|------|------|------|
| `GET /health` | 存活探针 | 免认证 |
| `GET /*`（非 `/api` 路径）| 静态前端托管（webRoot，默认 `webui/dist`，Vue 构建产物；防路径穿越）| 免认证 |
| `POST /api/config/reload` | 手动触发热重载 | Bearer + 限流 |
| `GET /api/config/schema` | 字段注册表下发（UI 动态表单供数） | Bearer + 限流 |
| `GET /api/config/{scope}` | 列出配置项名（scope 为 `protocols` \| `tags`）| Bearer + 限流 |
| `GET /api/config/{scope}/{name}` | 读取配置原文 | Bearer + 限流 |
| `PUT /api/config/{scope}/{name}` | 保存配置（校验→备份→原子写入→**自动热重载联动**）| Bearer + 限流 |
| `DELETE /api/config/{scope}/{name}` | 删除配置项（仅 protocols）| Bearer + 限流 |
| `GET /api/config/{scope}/{name}/backups` | 列出备份标签 | Bearer + 限流 |
| `POST /api/config/{scope}/{name}/backups/{tag}/restore` | 回滚到指定备份 | Bearer + 限流 |
| `GET /api/sim/status` | 仿真器清单 | Bearer + 限流 |
| `GET /api/sim/registers?proto=&start=&count=` | 读仿真数据区寄存器 | Bearer + 限流 |
| `POST /api/sim/registers` | 写仿真数据区寄存器（UI 手动改值） | Bearer + 限流 |
| `GET /api/data/latest[?device=]` | 引擎轮询最新值快照 | Bearer + 限流 |
| `GET /api/data/stream[?device=]` | SSE 实时推送（建连即推首帧，此后每 3s 全量快照）| Bearer 或 `?token=` + 限流 |
| `POST /api/data/write` | 单寄存器写（FC06 全链路：构建→发送→echo 校验→可选 read-back）| Bearer + 限流 |
| `GET /api/validate?scope=&name=` | 只读校验**磁盘现有内容**，返回结构化问题清单 | Bearer + 限流 |
| `POST /api/validate?scope=&name=` | 只读校验**请求体候选内容**（不落盘），返回结构化问题清单 | Bearer + 限流 |

> **非内置域**的 `/api/*`（`sim` / `data` / `validate` / 未来新增）由 App 层经 `SetExtHandler` 注入（§7.6）——内置域只有 `config`，另有免认证的 `/health`、`/metrics`；扩展路由仍在 token 鉴权之后。未注册 handler 时返回 `404 {"error":"ext api not available"}`。

## 7.3 配置管理 /api/config/*

- **保存链路（PUT）**：JSON 解析 → 字段级深度校验（ConfigDeepValidator）→ 备份现有文件 → 临时文件原子替换 → 成功后自动触发 `TriggerReload()`。
- **回滚**：新配置非法时 Save 直接失败且不落盘；已落盘配置可通过 backups/restore 回到任意备份标签。
- **reload 语义**：`POST /api/config/reload` 与 PUT 成功后的自动重载共用 `ConfigStore::Reload()`——若注册了 reload handler（生产模式必注册），则联动运行时（§7.4）；未注册时为空操作并返回成功。

示例：

```jsonc
// GET /api/config/protocols → ["modbus-tcp", ...]
// GET /api/config/schema   → 字段注册表 JSON (UI 动态表单供数)
// PUT /api/config/protocols/modbus-tcp  (body = 协议 JSON 原文)
// → 200 {"ok":true}   // 校验通过、已备份、已写盘、已触发 reload
// POST /api/config/reload → 200 {"ok":true}
```

### 7.3.1 只读校验 /api/validate

用途：WebUI 保存前「自检」、保存被拒后解释原因（见 [docs/webui.md](../../webui.md)）。

| 方法 | 语义 |
|------|------|
| `GET /api/validate?scope=protocols\|tags&name=<n>` | 校验**磁盘现有内容** |
| `POST /api/validate?scope=protocols\|tags&name=<n>`（body = 候选 JSON 原文）| 校验**待保存内容** |

响应（**校验未通过也返回 200** —— 请求本身成功，语义看 `body.ok`）：

```jsonc
{
  "ok": false, "scope": "protocols", "name": "s7-1200",
  "errorCount": 2, "warningCount": 0,
  "issues": [
    { "severity": "error", "ruleId": "frame.length_consistency",
      "subject": "WriteMultipleRegisters",
      "field": "operations.WriteMultipleRegisters.requestTemplate",
      "message": "操作 WriteMultipleRegisters 帧长一致性失败: 长度槽位值 10 ≠ 期望 9 ..." }
  ]
}
```

- **绝不写盘 / 不备份 / 不触发热重载** —— 这是与 `PUT /api/config/*` 的本质差别
- `severity`：`error` 阻断保存；`warning` 仅提示（原 `[WARN]` 条目）
- `ruleId`：稳定标识，现 **22 类**。第一批 9 类（帧长一致性 / 派生长度 expr / 长度偏移自检 / 设备→协议 / 标签→设备 / 标签→操作 / 模板文法 / 版本门禁 / 名称重复）；第二批 13 类（成帧参数 `Silence`·`LengthField`·`framing.type`、传输参数 `transport.type`·`host`·`serial`、TLS `caFile`/未实现告警、操作与协议命名、标签字段 `finalType`·`bitOffset`·`direction`）；未命中为 `unclassified`。规则表见 `src/Service/src/ValidationIssue.cpp`（新增一类 = 表里加一行）
- > `[webApi]` 段消息属全局 schema 域，不经 `protocols`/`tags` 域校验，**有意不建规则**（规则表宁缺勿假：建了也永不命中）
- `subject` / `field`：供 UI 点击定位（协议：模块/操作；标签：设备/标签）；`message` 始终是校验器原文，**不丢信息**
- 参数非法或缺失 → `400`；方法非 GET/POST → `405`；GET 目标配置不存在 → `404`

## 7.4 reload 运行时联动（仿真器 + 实时数据）

生产模式启动时注册 reload handler（App 层 `ApplyRuntimeSync`，与 E2E 共用同一代码路径）：

```cpp
// src/App/main.cpp → RunProduction
store.SetReloadHandler(
    [&configDir, &io, &sims, &simOwners, &latest, &protoStore]() {
        return ApplyRuntimeSync(configDir, io, sims, simOwners, latest,
                                protoStore, &std::cout);
    });
```

`ApplyRuntimeSync` 三步：

1. **重载配置目录**（`ConfigDirectoryLoader::Load`）——新配置非法即返回错误，运行态保持不变；
2. **全停重建仿真器**——对每个 `simulation.listenPort > 0` 的协议停掉旧实例并按新配置重建（注意 `ProtocolConfig` 拷入调用方持有的 `protoStore`，因 `SimulationServer` 持有元素引用）；端口迁移后旧端口立即不可连；
3. **清空实时快照**（`LatestValueStore::Clear()`）——旧标签集数据作废，由引擎按新一轮轮询重新积累。

由此形成"**UI 保存即生效**"闭环：UI 里改协议 JSON → PUT 成功 → 自动 reload → 仿真器按新端口/初值重启 → 设备侧立即感知。E2E Test 12 覆盖了端口迁移、初值生效、快照清空、旧端口关闭与坏配置保持旧态五个断言。

## 7.5 仿真数据 /api/sim/* 与实时数据面 /api/data/*

```cpp
// src/App/main.cpp → App 层注入的业务 handler (依赖倒置)

// GET  /api/sim/status → 仿真器清单 { "simulators": [ { "protocol": "...", "listenPort": N, "registerCount": N } ] }

// GET  /api/sim/registers?proto=P&start=0&count=16   (唯一仿真器时 proto 可省)
// → 200 { "start": 0, "values": [v0, v1, ...] }      // 16 位寄存器整数数组
// 越界 → 400 {"error":"地址范围越界 [0,65535]"}

// POST /api/sim/registers
// body: {"start":0, "values":[123], "protocol":"P"}   // protocol 可选, 多仿真器时指定
// → 200 {"ok":true}   // 写入即时对设备侧轮询可见

// GET  /api/data/latest[?device=<deviceId>]           // 引擎最新值快照
{
  "count": N,
  "tags": [
    { "name": "PLC-001.Temperature", "device": "PLC-001",
      "value": 25.7, "quality": "Good",
      "timestamp": 1746000000000, "changed": true }
  ]
}

// GET  /api/data/stream[?device=<deviceId>]            // SSE 实时推送
// 响应： 200 + Content-Type: text/event-stream + Cache-Control: no-store
//          + Connection: keep-alive (无 Content-Length, 连接不关)
// 帧格式： "data: {与 /latest 同构的 JSON}\n\n"
// 时序：   建连立即推首帧，此后每 3s 推一次全量快照
// 认证：   requireAuth 时 Bearer 头或 ?token= 查询参数二选一
//         (浏览器 EventSource 无法自定义请求头, 故提供 query 兜底);
//         限流仅在握手时判定一次。客户端断开/服务停止即清理会话。
//         断流后由浏览器 EventSource 自动重连

// POST /api/data/write                                // 单寄存器写
// body 标量:   {"tag":"R.W", "value":1234}           // value ∈ [0,65535]
// body 变长:   {"tag":"R.W15", "bytes":"01 0A 0B"}   // hex 偶数位, 空格/tab 忽略
// body 读回:   {"tag":"R.W", "value":1234, "readBack":true}  // P1 B: 写后读同地址校验
//      └─> bytes + readBack: 写后读 N 寄存器 (N = bytes/2), 长度对齐校验
//      └─> 不一致 → 503 ReadBackMismatch; 一致 → 200 {"ok":true}
// → 200 {"ok":true}    // echo 校验通过; readBack=true 时附加 read-back 一致
// → 400 ← JSON/缺字段/类型错/越界/readBack 非 bool/bytes 非法 hex
// → 404 未知路径; 405 非 POST;
// → 502 写失败 (标签/设备/协议未找到、连接失败、echo 失败、等待超时)
// → 503 read-back 不一致 (P1 B 新增, 与 502 写失败区分)
```

写路径线程模型：WebApi 线程收到请求后 `io.post` 全链（查标签/设备/协议 → GetOrCreateChannel → `TagReader::WriteOnce`），与轮询回调在 io 线程串行执行——消除跨线程 socket 操作与配置向量竞争；WebApi 线程经 promise/future 同步等待（上限 10s）。协议须声明名为 `WriteSingleRegister` 的操作，变量集 = tag.variables + `StartAddress` + `WriteValue`。

#### 写路径流程图

```
WebApi 线程                    io 线程                            metrics
   |                            |                                  |
   |- parse body (JSON)         |                                  |
   |- 校验 value ∈ [0,65535]    |                                  |
   |- std::make_shared<promise> |                                  |
   |- future = promise->get()   |                                  |
   |- io.post([captures...])    |                                  |
   |                            |- 1. 查 tag (按值取 *tag)         |
   |                            |- 2. 查 device -> {proto,timeout} |
   |                            |- 3. 按值拷贝 proto               |
   |                            |- 4. tagCopy (悬垂防护点 #1)      |
   |                            |- 5. gateway.GetOrCreateChannel  |
   |                            |    |- 已连 -> sync 调 cb          |
   |                            |    `- 未连 -> async 连后调 cb     |
   |                            |      (悬垂防护点 #2: GOC 异步)    |
   |                            |- 6. WriteOnce (FC06 构造+发送)   |
   |                            |    `- echo 校验                  |
   |                            |      (悬垂防护点 #3: WriteOnce 异|
   |                            |       步期间热重载替换 *tagsPtr)  |
   |                            |- 7. metrics.CounterInc           |
   |                            |      (kWritesTotal / kWriteFailure|
   |                            |       sTotal + Device label)     |
   |                            `- promise->set_value(...)         |
   |- future.wait_for(10s)      |                                  |
   `- return (status, json)     |                                  |
```

**3 个悬垂防护点**（按值拷贝的时点）：

| 点 | 拷贝对象 | 防护目的 | 代码行 |
|---|---|---|---|
| #1 | `tagCopy = *tag` | GOC 异步连接期间热重载可能替换 `*tagsPtr`；裸指针 `tag` 将悬垂 | [RuntimeGlue.cpp L473](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L473) |
| #2 | `proto`（按值拷贝）| 同上 — `*protosPtr` 异步期间可能被换 | [L456-469](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L456-L469) |
| #3 | `tagCopy` 再次捕获到 WriteOnce 闭包 | 闭包生命周期内 `*tagsPtr` 仍可能换 | [L473/477/488](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L473) |

**写路径错误码表**（[HandleWriteApi L520-553](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L520-L553) + [WriteViaGateway L432-507](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L432-L507)）：

| HTTP | 触发条件 | Error::Code | body 格式 |
|---|---|---|---|
| 200 | 写成功 + echo 校验通过 | — | `{"ok":true}` |
| 400 | JSON 解析失败 / 内部错误 | — | `{"error":"JSON 解析失败"}` / `{"error":"内部错误"}` |
| 400 | 缺字段 / 类型错 / 越界 | — | `{"error":"body 须为 ..."}` / `{"error":"寄存器值须在 [0,65535]"}` |
| 404 | 路径非 `/api/data/write` 开头 | — | `{"error":"unknown data endpoint"}` |
| 405 | method ≠ POST | — | `{"error":"method not allowed"}` |
| 502 | 标签未找到 | `TagNotFound` | `{"error":"标签未找到: <name>"}` |
| 502 | 设备未找到 | `DeviceNotFound` | `{"error":"设备未找到: <id>"}` |
| 502 | 协议未找到 | `ProtocolNotFound` | `{"error":"协议未找到: <name>"}` |
| 502 | 连接失败 / Echo 失败 / WriteOnce 失败 | （透传 cr.error() / wr.error()）| `{"error":"<msg>"}` |
| 502 | 10s 超时（future.wait_for 失败）| `Timeout` | `{"error":"写操作等待超时"}` |
| 503 | read-back 不一致（P1 B 销账项）| `ReadBackMismatch` | `{"error":"read-back 不一致: expected=…, got=…"}` |

> **注意**：`g_running` 标志位 + io 串行保证下，**热重载与 in-flight 写不会产生数据竞争**；但 WebApi 线程被卡死时（如 io 线程 hang）会触发 502 Timeout，需配合 [08_App §8.1 步骤 9](file:///d:/workSpaces/c#/MyProt-master/docs/modules/08_App.md) 主循环检查 `IsRunning()==false` 兜底。`quality` 为 `Good / Uncertain / Bad`；`value` 按 `finalType` 输出（ByteArray 转十六进制大写字符串）；`changed` 为本周期相对上周期是否变化。

## 7.5 write.readBack — 写后读回（P1 B 销账项）

**目的**：写应答 echo 校验只确认设备"收到"，不能确认"写入值正确落地"（某些从站会拒收部分字节、寄存器位宽截断、字节序错位等）。read-back 闭环校验写入值与设备实际状态一致。

**触发**：body 含 `"readBack": true`（默认 false；可与 `value` 或 `bytes` 组合）。

**协议契约**：

| 路径 | 读回行为 | 寄存器数推导 | 字节比较 |
|---|---|---|---|
| `value` + readBack | 用协议 `ReadHoldingRegisters` op 模板构造读请求 | 固定 1（注入 `RegisterCount=1`）| Modbus FC03 PDU 头 8 字节 + 字节计数 1 字节 + 寄存器值 2 字节布局；取首寄存器值 `[9..10]` 按 big-endian 解析 16-bit，与 `value` 比对 |
| `bytes` + readBack | 用写操作名 op 模板构造读请求 | `N = bytes总字节数 / 2`（注入 `RegisterCount`，必须偶数对齐；用写 op 是历史简化遗留）| 当前实现仅校验长度（`size() >= 9 + totalBytes`），逐字节深比较留给上层 |
| 协议无 `ReadHoldingRegisters` | 静默跳过 read-back | — | 直接回调成功，不退化为 503 |
| 读回失败 | 写 Echo OK + 读回失败 | — | 透传读回 error（如 Timeout → 502）|
| 读回不一致 | 写 Echo OK + 读回值与期望不等 | — | `ReadBackMismatch` → HTTP 503 |

**实现位置**：
- TagReader: [TagReader.cpp L237-L286](file:///d:/workSpaces/c#/MyProt-master/src/Gateway/src/TagReader.cpp#L237-L286)（WriteOnce read-back）+ [L342-L399](file:///d:/workSpaces/c#/MyProt-master/src/Gateway/src/TagReader.cpp#L342-L399)（WriteBytes read-back）
- RuntimeGlue: [RuntimeGlue.cpp L588-L595](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L588-L595)（readBack 解析 + 类型校验）+ [L632](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L632)（透传）+ [L648-L651](file:///d:/workSpaces/c#/MyProt-master/src/App/RuntimeGlue.cpp#L648-L651)（HTTP 错误码映射 503）
- 错误码: [Error.hpp L32](file:///d:/workSpaces/c#/MyProt-master/src/Core/include/MyProt/Core/Expected.hpp#L32)（`Error::Code::ReadBackMismatch`）
- 销账: [ADR-0007 §3 P1 B 行](file:///d:/workSpaces/c#/MyProt-master/docs/adr/0007-write-path-scope.md)（⏳ → ✅ 2026-08-28）
- E2E: [E2EMain.cpp Test 16](file:///d:/workSpaces/c#/MyProt-master/src/Tests/E2EMain.cpp)

**简化原则**：当前 read-back 复用写超时（`requestTimeoutMs`），不复用读轮询超时（`pollIntervalMs`）；不引入 device 级 readBackPolicy 配置（API 层 only，后续 KI 排期）。

## 7.6 扩展路由机制（依赖倒置）

`WebApiServer` 的路由白名单仅放行 `api/sim` 与 `api/data` 两个前缀给 `_ext` 回调——WebApi 模块不反向依赖 Simulation/Polling，业务 handler 全部由 App 层组装注入：

```cpp
// src/App/main.cpp → RunProduction
server.SetExtHandler([&sims, &latest, &io, &gateway,
                      &protosPtr, &devicesPtr, &tagsPtr](
        const std::string& m, const std::string& p,
        const std::string& b) {
    if (p.rfind("/api/data/write", 0) == 0) {
        return HandleWriteApi(m, p, b, io, gateway,
                              protosPtr, devicesPtr, tagsPtr);
    }
    if (p.rfind("/api/data", 0) == 0) {
        return HandleDataApi(m, p, latest);
    }
    return HandleSimApi(m, p, b, sims);
});

// SSE 流式路由: WebApi 层在请求解析后、普通路由前拦截 GET <pathPrefix>[?query],
// 建连即推首帧, 此后每 intervalMs 回调 provider 一次推送快照 (空串=跳过本轮);
// 会话由异步回调链持有 (socket+steady_timer), 写失败/Stop 即清理。
// provider 同样由 App 层注入，WebApi 不依赖业务快照来源。
server.SetStreamRoute("/api/data/stream", 3000,
    [&latest](const std::string& p) {
        return BuildLatestJson(latest, QueryParam(p, "device"));
    });
```

## 7.7 安全与通用约束

| 机制 | 行为 |
|------|------|
| 认证 | `requireAuth=true` 时校验 `Authorization: Bearer <token>`（常量时间比较），失败 `401`；token 取环境变量 `MYPROT_API_TOKEN`，缺失即构造 Fail-Fast |
| 限流 | 所有 `/api/*` 过客户端令牌桶（`rateLimitRps`/`rateLimitBurst`），超限 `429` |
| 绑定 | `bindAddress` 默认 `127.0.0.1` 仅环回（host 取配置、端口取 `--port`）；远程管理须显式改配并建议启用认证；非环回绑定且 `requireAuth=false` 时启动打印 WARN |
| 请求体上限 | 1 MB，超出 `413` |
| 响应头 | API 一律 `Cache-Control: no-store`；静态 hash 资产可缓存；统一 `X-Content-Type-Options: nosniff` |
| 错误码映射 | NotFound 统一 `404`；ParseError/ConfigError → `400`；Busy/CircuitOpen → `503`；其余 → `500` |

## 7.8 Web 管理前端（Vue 3）

`webui/` 为 Vite + Vue 3 SPA（构建产物由 WebApiServer 托管）。面板一览：

| 面板 | 文件 | 功能 | 消费端点 |
|------|------|------|------|
| 登录页 | LoginGate.vue | token 输入，localStorage 记忆 | — |
| 配置管理 | ScopeManager.vue + JsonEditor.vue | 协议/标签 CRUD、备份列表、一键回滚、左侧搜索框（item>5 显示）+ 设备 ID / 标签名实时重名校验 | `/api/config/*` |
| 仿真控制台 | SimPanel.vue | 仿真器卡片、寄存器表格读写、3s 轮询刷新）、单行改值即时生效 | `/api/sim/*` |
| 实时监控 | LivePanel.vue | 最新值快照表格、关键字过滤、变化行高亮、质量徽标（Good/Bad/Uncertain）、SSE 实时推送（断流自动重连）、手动刷新兜底、sparkline 30 点趋势图 | `/api/data/stream`、`/api/data/latest` |

---

> **文档版本**: v4.0（对齐实际实现：自研 asio HTTP 服务 + config CRUD/备份回滚 + `/api/sim|data` 扩展路由 + reload 运行时联动；取代 v3.0 的 httplib 设计稿）
> **上一篇**: [Polling 模块](./06_Polling.md)
> **下一篇**: [App 模块](./08_App.md)

# MyProtV2 ▸ WebApi 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Polling 模块](./06_Polling.md)
> **下一篇**: [App 模块](./08_App.md)

---

#### 多仿真器优先级（[SimApi.cpp L50-L187](file:///d:/workSpaces/c++/MyProt-master/src/App/SimApi.cpp#L50-L187)）

`/api/sim/*` 的查找与转发规则：

| 场景 | 请求 | 行为 | 代码定位 |
|---|---|---|---|
| **唯一仿真器** | `/api/sim/registers?start=0&count=4` | 自动取该仿真器（`sims.begin()->second`）| [L71](file:///d:/workSpaces/c++/MyProt-master/src/App/SimApi.cpp#L71) |
| **多仿真器 + 省略 proto** | 同上 | **400** `{"error":"存在多个仿真协议, 请以 ?proto= 指定"}` | [L67-L69](file:///d:/workSpaces/c++/MyProt-master/src/App/SimApi.cpp#L67-L69) |
| **多仿真器 + 显式 proto** | `/api/sim/registers?proto=modbus-tcp&start=0&count=4` | 按**协议名**匹配（即 `SimServerMap` 的键，也是 `protocolName`） | [L53-L61](file:///d:/workSpaces/c++/MyProt-master/src/App/SimApi.cpp#L53-L61) |
| **proto 不存在** | — | **404** `{"error":"无此仿真协议: <name>"}` | [L55-L59](file:///d:/workSpaces/c++/MyProt-master/src/App/SimApi.cpp#L55-L59) |

> **实例来源**：每个 `protocols/*.json` 的 `simulation.listenPort > 0` 对应一个仿真实例，因此同时启用几个协议就有几个仿真设备；`server.json` 的全局段仅在无协议级仿真时提供回退实例（[ADR-0017](../adr/0017-protocol-scoped-simulation-instances.md)）。
>
> **设计取舍**：唯一仿真器时省略 proto 是"历史 v3 残留"——v4 已要求显式，但仍保留降级以避免硬错误。`?proto=` query 形式由 [`QueryParam`](file:///d:/workSpaces/c++/MyProt-master/src/App/RuntimeGlue.cpp#L49-L65) 解析。

#### SSE 帧格式与 changed 优化

`/api/data/stream` 推送帧（[WebApiServer::SetStreamRoute](file:///d:/workSpaces/c++/MyProt-master/src/WebApi/src/WebApiServer.cpp) + [BuildLatestJson L51-L76](file:///d:/workSpaces/c++/MyProt-master/src/App/DataApi.cpp#L51-L76)）：

```
event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.234Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":true}, ...]}

event: snapshot
data: {"timestamp":"2026-08-27T10:00:01.534Z","tags":[{"name":"Tank.PV","value":1234,"quality":"Good","finalType":"UINT16","changed":false}, ...]}
```

**changed 字段语义**：本周期与上周期相比**值或质量发生变化**才为 `true`；判定由 `LatestValueStore::Update` 在写入时完成（首次写入 / 类型+值 / 质量变化），时间戳不参与。客户端可据此忽略重复行节省渲染开销（实测 1000 tags × 1Hz 时带宽约 250KB/s 含 changed=false；优化后约 8KB/s）。

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
WebApi 独立线程 (_io)                 App 层 provider
  |                                   |
  |-- HandleConnection 识别 GET <prefix>[?query]
  |-- StreamSession (socket + steady_timer) 交给异步回调链
  |-- HandleStream: 限流/认证 → 响应头 + 首帧
  |-- StreamWait: timer 到期 → StreamPush → StreamWait
  |                                   |
  |   客户端断开 / 写失败 / provider 异常
  |-- StreamClose: timer.cancel + socket shutdown/close
       (session shared_ptr 随回调链结束析构, 无悬挂)
```

**关键点**：
- **SSE provider 由 App 层注入**（[main.cpp L359-L362](file:///d:/workSpaces/c++/MyProt-master/src/App/main.cpp#L359-L362)）——`WebApiServer` 不反向依赖 `LatestValueStore`；`SetStreamRoute` 仅保存回调，WebApi 侧只持有 `StreamSession`（socket + timer）；
- **会话由异步回调链保活**：`StreamSession` 以 `shared_ptr` 在 `async_wait` / `async_write` 闭包间传递，客户端断开或 `Stop()` 取消立即触发 `StreamClose`，无需外部释放引用；
- **推送过程中热重载**：`ApplyRuntimeSync` 对同一个 `LatestValueStore` 调用 `Clear()`（不换对象），`provider` 裸指针始终有效；清空后到下一轮轮询完成前可能推出 `count=0` 的空快照，属预期语义，无悬挂。

#### 第二条事件流：`/api/mqtt/stream`（事件批量拉取）

`SetStreamRoute` 只能挂**一个**前缀，已被 `/api/data/stream` 占用；管理面 MQTT 测试面板需要第二条独立事件流，故新增 [`SetEventStreamRoute(prefix, intervalMs, provider)`](file:///d:/workSpaces/c++/MyProt-master/src/WebApi/src/WebApiServer.cpp)：

- `StreamSession` 自带 `intervalMs` 与 `provider`，两条流各自决定推送周期与数据来源，互不影响；
- `HandleConnection` 先匹配事件流前缀、再匹配数据流前缀，命中即走 `HandleStream`；
- 事件流 provider 返回**空串表示本轮无内容**，服务端跳过写帧（不发空 `data:`）。

帧格式（provider = `BuildMqttProbeStream`，每 200 ms 取一次 `MqttProbeHub::Poll(sid)`）：

```
data: {"state":"online","connected":true,"received":12,"dropped":0,"lastError":"",
       "events":[{"type":"message","topic":"myprot/dev/temp","payload":"{\"v\":21}",
                  "encoding":"utf8","qos":0,"retain":false,"ts":1756...,"bytes":9,"truncated":false}]}
```

- `state`：`connecting` | `online` | `error`；**会话已被回收/不存在时 provider 返回 `{"state":"closed",...}`**，前端据此关闭 EventSource 并提示重新开始；
- `events[]`：`type=status`（连接状态变更）或 `type=message`（订阅到的消息）；
- 载荷非法 UTF-8 或超过 8 KB 时 `encoding="hex"`，超限还带 `truncated=true`；
- 无事件且未到心跳间隔（5 s）时返回空串，服务端不写帧。

#### URL 查询参数解析（[QueryParam L49-L65](file:///d:/workSpaces/c++/MyProt-master/src/App/RuntimeGlue.cpp#L49-L65)）

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

    /// 内置路由清单 ("METHOD /pattern") — 供文档、覆盖检查与自测枚举
    std::vector<std::string> DescribeRoutes() const;
};

}} // namespace MyProt { namespace WebApi
```

### 路由表与统一分发（P2-3）

内置管理面路由统一注册到 `RouteTable`（`include/MyProt/WebApi/RouteTable.hpp`），不再使用 if 链：

- **注册**：`Add(method, pattern, handler)`，pattern 支持 `:name` 段参数（如 `/api/config/:scope/:name`）；
- **匹配**：先按「最具体模式」选择（字面量段数最多），再在该模式内比对 method —— 保证 `GET /api/config/reload` 返回 `405`（而不是被 `:scope` 吞成 `404`）；
- **统一前置处理**：静态资源（非 `/api`）→ 限流 → token 鉴权 → 路由表 → 扩展域 `SetExtHandler` → 404；`/health`、`/metrics` 与静态资源免认证；
- **错误映射**：配置域统一经 `ErrorResponse`（`StatusFromError` 取码）把 `Error::Code` 映射为 HTTP 状态码与 `{"error":...}`；
- **可枚举**：`DescribeRoutes()` 返回全部 `METHOD /pattern`，可用于生成 API 清单或覆盖检查。
- **CORS**：不启用。生产由本服务同源托管 `webui/dist`；开发态用 Vite `server.proxy` 转发 `/api`、`/health`，无需浏览器预检。

## 7.2 端点总表

| 方法 + 路径 | 功能 | 鉴权 |
|------|------|------|
| `GET /health` | 存活探针，返回 `{"status":"ok","authRequired":bool}`；前端据此决定是否展示登录页与「退出」 | 免认证 |
| `GET /metrics` | Prometheus 文本指标（text/plain v0.0.4）| 免认证 |
| `GET /*`（非 `/api` 路径）| 静态前端托管（webRoot，默认 `webui/dist`，Vue 构建产物；防路径穿越）| 免认证 |
| `POST /api/config/reload` | 手动触发热重载 | Bearer + 限流 |
| `GET /api/config/schema` | 字段注册表下发（UI 动态表单供数） | Bearer + 限流 |
| `GET /api/config/{scope}` | 列出配置项名（scope 为 `protocols` \| `tags`）| Bearer + 限流 |
| `GET /api/config/{scope}/{name}` | 读取配置原文 | Bearer + 限流 |
| `PUT /api/config/{scope}/{name}` | 保存配置（校验→备份→原子写入→**自动热重载联动**）| Bearer + 限流 |
| `DELETE /api/config/{scope}/{name}` | 删除配置项（仅 protocols；**不自动热重载**，需再调 `POST /api/config/reload`）| Bearer + 限流 |
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
| `GET /api/preview?scope=protocols&name=[&operation=]` | 试渲染**磁盘现有协议**：给出操作清单或一帧请求字节（只读） | Bearer + 限流 |
| `POST /api/preview?scope=protocols&name=` | 试渲染**请求体候选协议**（不落盘），返回请求字节与长度槽位核对 | Bearer + 限流 |
| `GET /api/preview/response?scope=protocols&name=&operation=&hex=` | 用**磁盘现有协议**解析给定响应字节（只读） | Bearer + 限流 |
| `POST /api/preview/response?scope=protocols&name=` | 用**请求体候选协议**解析响应字节（不落盘） | Bearer + 限流 |
| `POST /api/mqtt/probe` | 建立 MQTT 观测会话（网关代连 broker 并订阅）；200 `{"sid","stream"}` / 400 / 429 | Bearer + 限流 |
| `GET /api/mqtt/probe/{sid}` | 查询会话状态 JSON；无此会话 `404` | Bearer + 限流 |
| `DELETE /api/mqtt/probe/{sid}` | 停止并删除会话；无此会话 `404` | Bearer + 限流 |
| `POST /api/mqtt/probe/{sid}/publish` | 借该会话发布一条消息；`409` 表示连接未建立 | Bearer + 限流 |
| `GET /api/mqtt/stream?sid=` | SSE 事件流（第二条事件流路由，200 ms 轮询会话事件队列）| Bearer 或 `?token=` + 限流 |

> **非内置域**的 `/api/*`（`sim` / `data` / `validate` / `preview` / `mqtt` / 未来新增）由 App 层经 `SetExtHandler` 注入（§7.6）——内置域只有 `config`，另有免认证的 `/health`、`/metrics`；扩展路由仍在 token 鉴权之后。未注册 handler 时返回 `404 {"error":"ext api not available"}`。

## 7.3 配置管理 /api/config/*

- **保存链路（PUT）**：JSON 解析 → 深度校验（`SchemaChecker` / `SemanticChecker` / `TrialRenderer` 编排于 `ConfigDeepValidator`）→ 备份现有文件 → 临时文件原子替换 → 成功后自动触发 `TriggerReload()`。
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

用途：WebUI 保存前「自检」、保存被拒后解释原因（见 [docs/webui.md](../webui.md)）。

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

### 7.3.2 试渲染 /api/preview

用途：WebUI 协议工具栏「试渲染」——把「当前编辑器内容 + 操作 + 变量」渲染成一帧请求字节（见 [docs/webui.md](../webui.md)）。与网关共用 `RequestBuilder` 管线（`derivedLength` 注入 + `autoCompute`），**只读**：不写盘、不备份、不重载、不连设备。

| 方法 | 语义 |
|------|------|
| `GET /api/preview?scope=protocols&name=<n>[&operation=<op>]` | 渲染**磁盘现有内容**；省略 `operation` 时只返回 `operations` 清单 |
| `POST /api/preview?scope=protocols&name=<n>`（body 见下）| 渲染**候选内容**（`body.protocol` 优先于磁盘） |

```jsonc
// 请求体
{
  "protocol": "<候选 JSON 文本或对象>",   // 可选；缺省读磁盘
  "operation": "Read",
  "variables": { "StartByteAddress": 100, "ByteCount": 2 },  // 可选数值覆盖
  "raw": { "Data": "01 02 03" },          // 可选 {Name:raw} 载荷
  "payloadBytes": 3                       // raw 省略时的占位字节数 (0..1024)
}
// 响应 (渲染失败也返回 200, 语义看 ok)
{ "ok": true, "frameLength": 12, "hex": "AA BB ...", "ascii": "..",
  "errors": [], "warnings": [], "operations": ["Read", "..."],
  "variables": [{ "name": "UnitID", "source": "override", "value": 7 }],
  "lengthField": { "offset": 2, "width": 2, "byteOrder": "big",
                   "value": 6, "expected": 6, "ok": true } }
```

- `source` 层级：`protocol` \| `operation` \| `override` \| `default` \| `derived` \| `auto`；`auto` 变量在渲染期求值，`value` 恒为 `null`
- 长度槽位核对范围：只要 `framing.type=LengthField` 且槽位宽合法，**任何操作**（含读）都会核对；保存期 `TrialRenderer` 仅核对含 `{Name:raw}` 的写操作 —— 预览更严格，两者期望值算法共用 `Core::ExpectedLengthFieldValue`
- `raw` 的键名 = 操作 `requestTemplate` 中整元素 `{Name:raw}` 的 `Name`；响应不回传该清单（由前端从候选内容自行提取），省略即用 `payloadBytes` 占位
- `scope` 仅支持 `protocols`（其余 `400`）；`operation` 不存在时 `ok:false` 并在 `errors[0]` 列出可用操作
- `variables` 须为 0..4294967295 的整数，`raw` 须为 hex 字符串，`payloadBytes` 0..1024；非法 → `400`

> 路由为前缀匹配且**注册顺序即优先级**：`/api/preview/response` 必须先于 `/api/preview` 注册，否则会被后者吞掉。

### 7.3.3 响应解析预览 /api/preview/response

用途：WebUI 试渲染面板的「响应解析」页签——把设备返回的字节喂给网关**同一个** `Engine::ResponseParser`，核对成帧判定、`dataStartIndex`、字节序、类型转换与质量码（见 [docs/webui.md](../webui.md)）。**只读**：不写盘、不重载、不连设备。可选的 `converters` 用于预览 **tags.json 的标签级读后换算链**（v5 C1，`value' = value*k + b`），响应同时回报换算前值与实际使用的链。

| 方法 | 语义 |
|------|------|
| `GET /api/preview/response?scope=protocols&name=<n>&operation=<op>&hex=<hex>[&finalType=&bitOffset=&byteCount=&byteOrder=]` | 解析**磁盘现有协议**；`hex` 须紧凑或改用 POST（`QueryParam` 不做 URL 解码） |
| `POST /api/preview/response?scope=protocols&name=<n>`（body 见下）| 用**候选内容**解析（`body.protocol` 优先于磁盘） |

```jsonc
// 请求体
{
  "protocol": "<候选 JSON 文本或对象>",   // 可选；缺省读磁盘
  "operation": "Read",                  // 省略时只返回 operations 清单
  "hex": "03 04 00 2A 00 01",           // 响应字节; 空格/换行/逗号可省略
  "finalType": "UInt16",                // 可选; 缺省 UInt16
  "bitOffset": -1,                      // 可选; Bool 位提取 0..7, -1 = 不提取
  "byteCount": 0,                       // 可选; 0 = dataStartIndex 之后剩余全量
  "byteOrder": "LittleEndian",          // 可选; 缺省用协议/标签级解析顺序
  "converters": [{ "kind": "scale", "k": 0.1, "b": -40 }]  // 可选; 标签级换算链 (POST only)
}
// 响应 (语义失败也返回 200, 语义看 ok)
{ "ok": true, "operation": "Read", "errors": [], "warnings": [],
  "operations": ["Read"],
  "condition": { "checked": true, "expr": "resp[1] == 4", "ok": true },
  "byteOrder": "BigEndian", "dataStartIndex": 2,
  "frameLength": 6, "rawLength": 2, "rawHex": "00 2A",
  "value": { "type": "Double", "text": "-35.8", "numeric": -35.8 },
  "converters": [{ "k": 0.1, "b": -40 }],
  "valueBeforeConversion": { "type": "UInt16", "text": "42", "numeric": 42 },
  "quality": "Good", "timestamp": 1730000000 }
```

- 校验顺序：协议 JSON → `operation` 存在性 → `hex` 合法性 → `bitOffset` 范围 → `dataStartIndex` 越界 → `validCondition` → 类型转换
- `validCondition` 不满足 → `ok:false` 且 `errors[0]` 指出条件表达式（不返回看似正常的值）；未声明 `validCondition` 时 `condition.checked=false`
- `byteCount` 超出数据区剩余量 → 退化为剩余全量并写入 `warnings[]`（与运行时一致）；`dataStartIndex` 恰好等于帧长 → 数据区为空，报「解析失败」
- `converters` 项 `{kind:"scale", k, b}` 与 `tags.json` 同形：按序应用后结果统一为 `Double`；`kind` 仅支持 `scale`，`k=0` / 非有限数值 / 非数值型 `finalType` → `ok:false`（规则 13 语义提前到预览期）；仅 POST 支持
- `value` 仅在类型转换成功时非 `null`；未提供 `converters` 时 `valueBeforeConversion` 为 `null`；非数值类型（`Bool` / `String` / `ByteArray`）的 `numeric` 为 `null`
- 缺 `hex` → `400`；`bitOffset` 非 `-1|0..7`、`byteCount` 超 `0..4294967295`、`protocol` 类型非法 → `400`；方法非 GET/POST → `405`；GET 目标配置不存在 → `404`

## 7.4 reload 运行时联动（仿真器 + 实时数据）

生产模式启动时注册 reload handler（App 层 `ApplyRuntimeSync`，与 E2E 共用同一代码路径）：

```cpp
// src/App/main.cpp → RunProduction
store.SetReloadHandler([&configDir, &ctx]() {
    return ApplyRuntimeSync(configDir, ctx, &std::cout);
});
```

`ApplyRuntimeSync` 步骤：

1. **重载配置目录**（`ConfigDirectoryLoader::Load`）——新配置非法即返回错误，运行态保持不变；
2. **构建新一代快照并收集仿真实例**——加载产物直接 move 入内；逐协议取 `simulation.listenPort > 0`，无协议级配置时回退 `server.json`（承载 0 号协议）；**此阶段不停止旧实例、不发布新快照**；
3. **端口冲突前置校验**——两个启用实例同端口即返回 `ConfigError`，旧仿真器与旧快照保持不变；校验通过后才停掉旧实例并同步发布 `ctx.snapshot`（管理面读写立即看到新配置），随后逐个启动新实例；
4. **实时快照按新标签集收敛**（`LatestValueStore::Retain(keepTagNames)`）——已删除标签立即移除，未变标签保留上次值与变化检测基线（保存瞬间不整表清空）；
5. **`io.post` 重装配（设备级 diff）**——在 io 线程依次执行：`ChannelManager::ApplyDevices()`（连接身份未变的设备保留通道与会话，`Disabled` 设备重建会话以恢复）→ `latest.Retain()` → `startEngine()`（生产绑定 `PollingEngine::ApplySnapshot`，未变设备保留轮询组）。不再调用 `gateway.Shutdown()` / `ResetDevices()` / `engine.Stop()`。

由此形成"**UI 保存即生效**"闭环：UI 里改协议 JSON 的 `simulation` → PUT 成功 → 自动 reload → 对应仿真器按新端口/初值重启（增加/删除协议级仿真段即增减仿真设备）→ 设备侧立即感知。E2E Test 12 覆盖端口迁移与坏配置保持旧态；Test 16d 覆盖两协议各一实例与旧回退端口关闭。

## 7.5 仿真数据 /api/sim/* 与实时数据面 /api/data/*

```cpp
// src/App/main.cpp → App 层注入的业务 handler (依赖倒置)

// GET  /api/sim/status → 仿真器清单 { "simulators": [ { "protocol": "<protocolName>", "listenPort": N, "registerCount": N } ] }

// GET  /api/sim/registers?proto=<protocolName>&start=0&count=16   (唯一仿真器时 proto 可省)
// → 200 { "start": 0, "values": [v0, v1, ...] }      // 16 位寄存器整数数组
// 越界 → 400 {"error":"地址范围越界 [0,65535]"}

// POST /api/sim/registers
// body: {"start":0, "values":[123], "protocol":"<protocolName>"}   // protocol 可选, 多仿真器时指定
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
   |                            |       步期间热重载替换快照)     |
   |                            |- 7. metrics.CounterInc           |
   |                            |      (kWritesTotal / kWriteFailure|
   |                            |       sTotal + Device label)     |
   |                            `- promise->set_value(...)         |
   |- future.wait_for(10s)      |                                  |
   `- return (status, json)     |                                  |
```

**3 个悬垂防护点**（快照保活 + 按值拷贝的时点）：

| 点 | 保活对象 | 防护目的 | 代码位置 |
|---|---|---|---|
| #1 | `snapshot`（捕获 `RuntimeSnapshotPtr`） | GOC/WriteOnce 异步期间热重载整代替换；shared_ptr 保活旧代向量，裸指针不会悬垂 | [WriteService.cpp](file:///d:/workSpaces/c++/MyProt-master/src/Gateway/src/WriteService.cpp) |
| #2 | `proto`（按值拷贝 → `shared_ptr<const ProtocolConfig>`） | 写请求跨异步边界；按值持有一份协议定义 | 同上 |
| #3 | `tagCopy` 再次捕获到 WriteOnce 闭包 | 闭包生命周期内不再依赖快照内的元素地址 | 同上 |

**写路径错误码表**（[HandleWriteApi](file:///d:/workSpaces/c++/MyProt-master/src/App/DataApi.cpp) 做 HTTP 映射 + [WriteService::Write](file:///d:/workSpaces/c++/MyProt-master/src/Gateway/src/WriteService.cpp) 产出 `Error::Code`）：

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

> **注意**：`g_running` 标志位 + io 串行保证下，**热重载与 in-flight 写不会产生数据竞争**；但 WebApi 线程被卡死时（如 io 线程 hang）会触发 502 Timeout，需配合 [08_App §8.1 步骤 9](file:///d:/workSpaces/c++/MyProt-master/docs/modules/08_App.md) 主循环检查 `IsRunning()==false` 兜底。`quality` 为 `Good / Uncertain / Bad`；`value` 按 `finalType` 输出（ByteArray 转十六进制大写字符串）；`changed` 表示该标签相对上一次写入是否变化（`LatestValueStore` 统一判定，时间戳不参与）。

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
- TagReader: [TagReader.cpp L237-L286](file:///d:/workSpaces/c++/MyProt-master/src/Gateway/src/TagReader.cpp#L237-L286)（WriteOnce read-back）+ [L342-L399](file:///d:/workSpaces/c++/MyProt-master/src/Gateway/src/TagReader.cpp#L342-L399)（WriteBytes read-back）
- DataApi: [DataApi.cpp L704-L711](file:///d:/workSpaces/c++/MyProt-master/src/App/DataApi.cpp#L704-L711)（readBack 解析 + 类型校验）+ [L757](file:///d:/workSpaces/c++/MyProt-master/src/App/DataApi.cpp#L757)（透传）+ [L769-L772](file:///d:/workSpaces/c++/MyProt-master/src/App/DataApi.cpp#L769-L772)（HTTP 错误码映射 503）
- 错误码: [Error.hpp L32](file:///d:/workSpaces/c++/MyProt-master/src/Core/include/MyProt/Core/Expected.hpp#L32)（`Error::Code::ReadBackMismatch`）
- 销账: [ADR-0007 §3 P1 B 行](file:///d:/workSpaces/c++/MyProt-master/docs/adr/0007-write-path-scope.md)（⏳ → ✅ 2026-08-28）
- E2E: [E2EMain.cpp Test 16](file:///d:/workSpaces/c++/MyProt-master/src/Tests/E2EMain.cpp)

**简化原则**：当前 read-back 复用写超时（`requestTimeoutMs`），不复用读轮询超时（`pollIntervalMs`）；不引入 device 级 readBackPolicy 配置（API 层 only，后续 KI 排期）。

## 7.6 扩展路由机制（依赖倒置）

`WebApiServer` 不硬编码业务前缀：除内置 `api/config` 与免认证的 `/health`、`/metrics` 外，其余 `/api/*` 一律交 `_ext` 回调——WebApi 模块不反向依赖 Simulation/Polling/Northbound，业务 handler 全部由 App 层组装注入：

```cpp
// src/App/main.cpp → RunProduction
HttpRouter extRoutes;
extRoutes.Add("/api/data/write",
    [](const AppContext& c, const HttpRequest& r) { return HandleWriteApi(c, r); });
extRoutes.Add("/api/data",
    [](const AppContext& c, const HttpRequest& r) { return HandleDataApi(r, *c.latest); });
extRoutes.Add("/api/sim",
    [](const AppContext& c, const HttpRequest& r) { return HandleSimApi(r, *c.sims); });
// MQTT 观测会话 (需按值捕获引擎对象引用; hub 不进 AppContext)
extRoutes.Add("/api/mqtt/probe",
    [&mqttProbe](const AppContext&, const HttpRequest& r) {
        return HandleMqttProbeApi(mqttProbe, r);
    });

server.SetExtHandler([&ctx, &extRoutes](
        const std::string& m, const std::string& p, const std::string& b) {
    HttpRequest req; req.method = m; req.path = p; req.body = b;
    return extRoutes.Dispatch(ctx, req);
});

// SSE 流式路由: WebApi 层在请求解析后、普通路由前拦截 GET <pathPrefix>[?query],
// 建连即推首帧, 此后每 intervalMs 回调 provider 一次推送快照 (空串=跳过本轮);
// 会话由异步回调链持有 (socket+steady_timer), 写失败/Stop 即清理。
// provider 同样由 App 层注入，WebApi 不依赖业务快照来源。
server.SetStreamRoute("/api/data/stream", 3000,
    [&latest](const std::string& p) {
        return BuildLatestJson(latest, QueryParam(p, "device"));
    });

// 第二条事件流 (MQTT 观测会话): 自己的周期与 provider, 与数据流互不影响
server.SetEventStreamRoute("/api/mqtt/stream", 200,
    [&mqttProbe](const std::string& p) {
        return BuildMqttProbeStream(mqttProbe, p);
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
| MQTT 测试 | MqttTestPanel.vue | 网关代连 broker（TCP）并订阅过滤器，SSE 回推消息与连接状态，可借会话回环发布 | `/api/mqtt/probe*`、`/api/mqtt/stream` |

---

> **文档版本**: v4.0（对齐实际实现：自研 asio HTTP 服务 + config CRUD/备份回滚 + `/api/sim|data` 扩展路由 + reload 运行时联动；取代 v3.0 的 httplib 设计稿）
> **上一篇**: [Polling 模块](./06_Polling.md)
> **下一篇**: [App 模块](./08_App.md)

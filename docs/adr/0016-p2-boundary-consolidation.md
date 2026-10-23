# ADR-0016：P2 边界收敛（校验拆分 · 写路径下沉 · 路由表 · 仿真单协议）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-23（实现与回归完成） |
| 日期 | 2026-10-23 |
| 相关文档 | [04_Service](../modules/04_Service.md) · [05_Gateway](../modules/05_Gateway.md) · [07_WebApi](../modules/07_WebApi.md) · [10_Simulation](../modules/10_Simulation.md) · [ADR-0010](./0010-vs2015-cpp11-toolchain.md)（C++11/VS2015 约束） |
| 提案来源 | 2026-10 架构评审 P2：巨石校验器、App 承载写业务、WebApi 多套路由分发、仿真静默取 0 号协议 |

---

## 背景

评审在 P2 列出四项结构性偏差：

1. `ConfigDeepValidator.cpp` 单文件约 2300 行，解析、结构规则、语义规则与写帧试算混居，修改任一规则都要在巨型文件中定位；
2. `/api/data/write` 的写业务（标签解析、可写性判定、编码、读回校验）位于 App 层 `DataApi.cpp`，与 HTTP 适配耦合，单元测试无法脱离 HTTP 覆盖；
3. `WebApiServer::Route` 以 if 链/前缀表/SSE 特例三套机制分发，端点不可枚举，method 不匹配时状态码语义不一致；
4. 仿真启用时静默取 `protocols[0]`，其余协议既不参与也无任何提示。

## 决策

### 2.1 校验器按职责拆分（P2-1）

- `ConfigDeepValidator.cpp` 只保留构造函数、`validateProtocol` 规则串联、警告合并与 JSON 文本入口；
- 规则实现分居四个 TU：
  - `ProtocolJsonParser.cpp` — JSON → POCO（`detail::Parse*Impl`）；
  - `SchemaChecker.cpp` — 单字段/单块结构规则（版本、传输、成帧、握手、设备、韧性、WebApi）；
  - `SemanticChecker.cpp` — 模板文法、派生长度、标签与配置根跨文件引用；
  - `TrialRenderer.cpp` — 写帧试算与成帧长度槽位比对（原 `FrameConsistencyCheck.cpp`）。
- 新文件 `src/Service/src/ConfigInternals.hpp` 声明跨 TU 的 `detail` 契约，**不进入** `include/MyProt/Service/` 公共头；
- 对外 API（`ConfigValidator` 的六个静态入口与两个 `validate*`）保持不变。

### 2.2 写路径下沉 Gateway（P2-2）

- 新增 `Gateway::WriteService`（`WriteRequest{tagName,value,bytes,readBack}` + `Write()`）；
- 写业务全部迁入 `src/Gateway/src/WriteService.cpp`：快照查询 → 可写性判定 → 标量/变长编码 → `WriteOnce`/`WriteBytes` → 可选读回；
- 线程模型不变：`io.post` → promise/future（10s 上限），与轮询回调串行；
- App 的 `HandleWriteApi` 只保留 HTTP body 解析与状态码映射；
- 写路径指标计数随实现迁移，仍在 `WriteService` 内。

### 2.3 统一内置路由表（P2-3）

- 新增 `WebApi::RouteTable`：`Add(method, pattern, handler)`，pattern 支持 `:param`；
- `Dispatch` 分两遍：先求「最具体模式」（字面量段数最多），再在该模式内比对 method；
  由此 `GET /api/config/reload` 返回 `405`，不会被 `/api/config/:scope` 吞成 `404`；
- 统一前置顺序：静态资源（非 `/api`，免认证免限流）→ 限流 → token 鉴权 → 路由表 → 扩展域 `SetExtHandler` → 404；
- `/health`、`/metrics` 作为免认证路由注册，`DescribeRoutes()` 可枚举全部内置端点；
- CORS 不启用：生产同源托管 `webui/dist`，开发态由 Vite `server.proxy` 转发，无跨域预检需求。

### 2.4 仿真显式限制为单协议（P2-4）

> **2026-10-23 更新：本节已被 [ADR-0017](./0017-protocol-scoped-simulation-instances.md) 取代。**
> 仿真现支持协议级多实例（`protocols/*.json` 的 `simulation`），`server.json`
> 单实例仅作为无协议级配置时的回退；端口冲突在启动前拒绝。以下为历史决策原文。

- 一份 `server.simulation` 只承载 0 号协议；多协议并存时启动日志逐个点名被忽略的协议并给出多实例建议；
- `/api/sim/status` 只报告实际运行的仿真器；未承载协议名请求 `?proto=` 返回 `404 无此仿真协议`；
- 不新增 `simulation.protocol` 字段：多协议仿真通过多实例 + 不同 `listenPort` 实现。

## 影响文件

| 层 | 文件 |
|----|------|
| Service | `ConfigDeepValidator.cpp`（编排）、`ProtocolJsonParser.cpp`、`SchemaChecker.cpp`、`SemanticChecker.cpp`、`TrialRenderer.cpp`、`ConfigInternals.hpp` |
| Gateway | `include/MyProt/Gateway/WriteService.hpp`、`src/WriteService.cpp` |
| WebApi | `include/MyProt/WebApi/RouteTable.hpp`、`WebApiServer.*` |
| App | `DataApi.cpp`（仅 HTTP 适配）、`RuntimeGlue.cpp`（多协议告警） |
| 构建 | 顶层 `CMakeLists.txt`、`MyProt.Service.vcxproj(.filters)`、`MyProt.Gateway.vcxproj(.filters)`、`MyProt.WebApi.vcxproj(.filters)` |

## 验证

- Linux/CI：`cmake --build build-linux -j4` 通过；`ctest --output-on-failure` 5/5 通过（E2E 约 3.1s）；
- E2E 新增断言：路由表 10 条可枚举、`GET /metrics` 免认证 200、`GET /api/config/reload` 返回 405；
- 既有配置 CRUD、鉴权、SSE、写路径、仿真闭环用例全部保持通过。

## 后果与约束

- 规则新增点固定在 `SchemaChecker`（结构）或 `SemanticChecker`（语义），禁止回写进编排层；
- 写端点新增字段时只改 `WriteRequest` 与 `WriteService`，App 不承载业务；
- WebApi 新增内置端点必须走 `RouteTable::Add` 并补充 `DescribeRoutes()` 覆盖断言；
- 仿真多协议能力已由 [ADR-0017](./0017-protocol-scoped-simulation-instances.md) 定义（协议级 `simulation` + `server.json` 回退），本 ADR 不再约束该方向。

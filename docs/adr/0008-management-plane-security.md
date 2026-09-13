# ADR-0008：管理面（WebApi）安全加固

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** — 2026-08-03（提议 2026-08-03）|
| 日期 | 2026-08-03 |
| 相关文档 | [architecture/04](../architecture/04_Error_Shutdown_Security.md) §9（安全设计）· [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0007](./0007-write-path-scope.md)（写端点） |

---

## 背景

设备侧（南向）安全有设计：TLS 传输（`TlsTransportConfig`）、设备级认证（握手模板变量）。但**管理面（北向 WebApi）明显更弱**：

- **仅单一静态 bearer token**：`WebApiHost::WithAuthToken(std::string)`，为空则完全不鉴权。无失效、无轮换、无每客户端区分。
- **WebApi 自身无 TLS**：cpp-httplib 默认 `httplib::Server`（明文 HTTP）。token 在明文链路上裸奔，工业网关管理口暴露在网络里即可被嗅探。
- **无限流**：`/api/data/write` 与 `/admin/reload`（触发配置重载）无任何速率限制，可被刷爆或反复触发重载抖动。
- **绑定地址未定义**：默认监听所有接口（`0.0.0.0`）还是仅环回，文档未说明。

管理面是控制网关的入口（读全部测点、触发配置重载、写设备），其安全等级**不应低于设备侧**。

## 决策驱动因素

- **管理面至少与设备侧同等**：设备侧有 TLS，管理面也应有。
- **v1 可落地、不过度设计**：不引入完整 IAM/RBAC/OAuth，但要堵住明文、弱鉴权、无限流三个硬伤。
- **默认安全**：缺省配置应偏向安全（如默认仅环回、生产必须设 token），而非偏向便利。
- **与现有栈一致**：cpp-httplib 提供 `httplib::SSLServer`，复用而非引入新框架。

## 决策

### 1. 管理面 TLS（`httplib::SSLServer`）

- WebApi 支持可选 TLS：配置管理面证书/私钥后，`WebApiHost` 使用 `httplib::SSLServer` 而非 `httplib::Server`，其余路由与处理器不变。
- 新增管理面安全配置块 `webApi`（配置文件顶层，见 §5），字段：`certFile` / `keyFile`（二者齐备即启用 TLS）。
- **默认（未配证书）= 明文 HTTP**，但启动日志给出 **Warning**（管理面未启用 TLS，仅建议用于本机/受信任网络）。

### 2. 鉴权强化

- 保留 bearer token 机制，但：
  - **常量时间比较**（避免时序侧信道），不用 `==`。
  - **生产模式强制 token**：若 `webApi.requireAuth = true`（生产建议）且 token 为空 → 启动 **Fail-Fast**（`ConfigError`）。开发模式（`requireAuth = false`）可空 token，但启动 Warning。
  - token 来源支持环境变量（`MYPROT_API_TOKEN`），避免写进配置文件/日志；日志中一律脱敏。
- v1 不引入多用户/RBAC/token 轮换（列为未来扩展）。

### 3. 限流（敏感端点）

- 对**状态变更类**端点做令牌桶限流（per 客户端 IP）：`/admin/reload`、`/api/data/write`。
- 默认：`rateLimitRps = 5`（每秒 5 次）、突发 `burst = 10`；超限返回 `429 Too Many Requests`。
- 只读端点（`/api/tags` 等）与探针（`/api/health*`、`/metrics`）默认不限流（可配置）。

### 4. 绑定地址与安全头

- **默认仅环回**（`127.0.0.1`）：管理口默认只对本机开放；需远程管理时显式配 `webApi.bindAddress`（如 `0.0.0.0`），并强烈建议同时启用 TLS + token。
- 响应增加基础安全头：`X-Content-Type-Options: nosniff`、`Cache-Control: no-store`（测点数据不缓存）。

### 5. 配置归属

管理面安全参数归入配置文件顶层 `webApi` 块（与 `resilience` 同级，由 Config_Schema 收编）：

| 字段 | 默认 | 说明 |
|------|------|------|
| `bindAddress` | `"127.0.0.1"` | 监听地址；远程管理需显式设 `0.0.0.0` |
| `certFile` / `keyFile` | 空 | 齐备则启用 TLS（SSLServer） |
| `requireAuth` | `false` | true 且 token 缺失即 Fail-Fast（生产建议 true） |
| `rateLimitRps` / `rateLimitBurst` | 5 / 10 | 敏感端点令牌桶限流 |

> token 本身不经配置块明文存储，优先取环境变量 `MYPROT_API_TOKEN`；`requireAuth = true` 且环境变量为空时启动失败。

## 影响

- `architecture/04` §9：新增 §9.4「管理面安全」小节，引用本 ADR（TLS / 鉴权 / 限流 / 绑定）。
- `modules/07_WebApi.md`：`WebApiHost` 支持 TLS（SSLServer）、常量时间 token 校验、限流中间件、bindAddress；端点表注明敏感端点限流；启动 Warning/Fail-Fast 行为。
- `modules/08_App` / `modules/09_Initialization`：`webApi` 配置块的加载与 `WebApiHost` 构造接线。
- `Config_Schema.md`：`webApi` 块纳入配置文件顶层（与 `resilience` 同级），补字段说明与校验（bindAddress 合法性、cert/key 成对、requireAuth 与 token 关系）。`webApi` 为可选块，缺省即全部默认值，**不触发** `schemaVersion` 代际递增（ADR-0005）。

## 预留扩展

- 多客户端 token / 按 token 权限 scope（读 vs 管理 vs 写）。
- token 轮换与过期、审计日志（谁在何时触发了 reload/write）。
- mTLS 客户端证书鉴权（管理面双向认证）。

## 复核触发条件

- 部署环境要求远程管理常态化（→ TLS + 强鉴权从"建议"升为"强制"）。
- 出现需要细粒度权限（RBAC）的多租户管理需求。

---

> **相关文档**: [architecture/04](../architecture/04_Error_Shutdown_Security.md) · [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0007](./0007-write-path-scope.md) · [文档索引](../README.md)

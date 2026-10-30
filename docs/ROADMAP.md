# 扩展路线图

> 本文件归档**尚未实装**的扩展项。主文档遇到对应字段会指向此处。
>
> - **优先级图例**：P0=必做，P1=应当，P2=可选，P3=远期
> - **状态图例**：📋 待评审 | 🔧 设计稿完整 | 🚧 实现中 | ⏸ 搁置（理由：依赖外部 / 优先级低）

---

## 1. 扩展候选

| # | 名称 | 优先级 | 状态 | 位置 / 依赖 | 备注 |
|:-:|------|:--:|:--:|------|------|
| 1 | TLS 实装（`TlsChannel`） | P0 | 📋 | `transport/TlsChannel.{h,cpp}` | 需 OpenSSL；当前 stub 返 `NotImplemented`；启用后 OpenSSL 依赖从「可选」转「必需」 |
| 2 | CAN 总线实装 | P1 | 📋 | `transport/CanChannel.{h,cpp}` | 需 socketcan (linux) / PcanBasic (win)；ISO-TP 分段 |
| 3 | 无响应写（单向 RTU） | P1 | ✅ 2026-10-24 已销账 | 写事务分支 | 已由操作级 `expectResponse=false` 交付：发完即成功、不读应答；读标签引用与 `readBack=true` 在校验期/运行时拒绝。见 [ADR-0018](./adr/0018-one-way-send-operation.md) |
| 4 | W3C Trace 接入 | P1 | 🔧 | `TagValue.traceId` / `parentSpanId` | 当前以 `requestId` 复用做端到端关联 |
| 5 | 端点细分限流（按 path 区分令牌桶） | P2 | 🔧 | `webApi.rateLimitRps` | 当前为全局令牌桶 |
| 6 | `Tls` 字段补 `defaultPort` | P3 | 📋 | `transport.Tls` | Tls 未实装 → 缺省 0 |
| 7 | alertSink / webhook 端点 | P3 | 🔧 | `ServerConfig` 新字段 | `server.json` 已留位 |
| 8 | `operations[].converters` 后处理链 | P1 | ✅ 2026-09-17 部分销账 | `ResponseParser` | 标签级 `converters` 读后换算链已实装（v1 仅 scale：`value'=value*k+b`，按序应用，结果 Double；仅数值型读标签，规则13 守门；写路径逆向换算未做，位提取仍走 bitOffset 一等字段）。单测 ResponseParserTest.Converters* ×3 |
| 9 | `{Crc:*}` 子范围校验 | P3 | 📋 | `Engine` | 当前为全字段校验 |
| 10 | 协议包下载安装工具 | P3 | 📋 | `scripts/` | 需 cli 设计 |
| 11 | 协程化（asio → C++20 coroutine） | P3 | ⏸ | 全局 | 需工具链升级（脱离 VS2015） |
| 12 | 滚动日志 / 日志归档 | P2 | 🔧 | `Service/ConfigStore` | 与 FileWatcher 配合 |
| 13 | FileWatcher 强化（debounce + 链路通知） | P2 | 🔧 | `Service/ConfigStore` | 当前已支持热重载，未做事件总线 |
| 14 | 配置迁移工具（schema 升级） | P3 | 📋 | `scripts/migrate` | 当前不自动迁移 |
| 15 | `TagValue.remoteTimestamp` / `unit` 字段 | P2 | 🔧 | `TagValue` | 需协议层时间戳抽象；当前仅「网关时间」 |
| 16 | 上报过滤（变化上报 / 死区） | P2 | 🚧 | `Polling` 发布层 | 原 `deadband` / `reportMode` 字段已删除（无消费落点，见 ADR-0003）。`changed` 已由 `LatestValueStore::Update` 在写入时统一计算（首次 / 质量码 / 类型+值，时间戳与 `requestId` 不参与，见 [modules/06 §变化检测](./modules/06_Polling.md#变化检测)），MQTT 北向以 `publishMode=changed` 消费该位；HTTP/SSE 的「只推变化」与可配置死区仍待设计 |
| 17 | 模板**文本行原语**（`$"..."` 正文 + `$(Var)` 文本替换） | P2 | 📋 | `Engine` 文法 + `Service` 校验 | 面向文本/JSON 协议（如仙工 SEER：正文为 JSON，参数需文本替换）；当前以"JSON 的 hex 编码"绕过（见 [protocols/seer.md](./protocols/seer.md) §3.2/§6）；可顺带设计"响应文本 → 数值/字段"解码 |
| 18 | 南向 MQTT 请求-应答（`MqttChannel`） | P2 | 📋 | `Transport` 新通道 | 把 MQTT 作为设备侧协议：需多路复用 + `requestId` correlation + 超时映射；与 `IChannel` 单在途语义冲突，需先定通道契约（见 [modules/11 §11.1](./modules/11_Northbound.md)） |
| 19 | 南向 MQTT 订阅上报（设备主动推送） | P2 | ✅ 2026-10-25 已销账 | `src/Southbound` | 新增 `IInboundTransport`（MQTT 订阅 / TCP 监听）+ `device.ingest` + 标签 `source=push`；事件流入站经 `TagValueSink` 写入 `LatestValueStore`，不经轮询引擎。见 [modules/12](./modules/12_Southbound.md) / [ADR-0019](./adr/0019-southbound-push-ingest.md)。TCP 入站与 MQTT 订阅同一入口；二进制载荷/入站鉴权仍待扩展 |
| 20 | UDP 数据报传输（`UdpChannel`） | P1 | ✅ 2026-10-27 已交付 | `transport/UdpChannel.{hpp,cpp}` | 无连接、**一个数据报=一帧**（framing 退化为接收上限保护），复用整条字节管线；`TransportType::Udp` + 端点缓存 `udp:host:port`。见 [ADR-0002 §7](./adr/0002-transport-abstraction.md)。单播请求-应答已覆盖；组播 / UDP 入站监听未做 |
| 21 | 南向 HTTP 轮询设备 REST API | P1 | 📋 | `Transport` / `App` 工厂 | 网关作 HTTP 客户端定时 GET/POST 设备取数；响应约定 `[{tag,value,ts}]` JSON 数组，复用 `PayloadMapper::MapJson` + `TagValueSink`。属应用集成层，不是 `IChannel` 字节流语义 → 届时另立 ADR |

---

## 2. 远期方向

- **工具链升级**：脱离 VS2015 v140 → VS2019 / clang-cl，解锁 C++17/20
- **多 io_context 线程 run**：若切多线程 run，需重新引入显式 strand 或写互斥
- **插件化协议包**：协议 JSON + 动态库，动态加载新协议无需重新编译
- **分布式网关**：跨节点状态同步、主备切换
- **管理面 RBAC**：当前仅 token 鉴权，未来多用户 / 角色

---

## 3. 评审流程

任何新功能加入本路线图前需通过：

1. **ADR 化**：写 [adr/](./adr/) 决策文件，说明动机、备选、决策
2. **影响面评估**：是否改 `schemaVersion`？是否改 [Config_Schema.md](./Config_Schema.md)？是否影响各模块接口？
3. **回退方案**：若启用后出问题，能否通过配置层 disable？
4. **测试覆盖**：E2E 套件（`src/Tests/E2EMain.cpp`）新增用例

---

## 4. 关联索引

- [docs/README.md](./README.md) — 主文档入口
- [docs/adr/](./adr/) — 架构决策记录

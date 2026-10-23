# ADR-0017：协议级仿真实例（多仿真设备）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-23 |
| 日期 | 2026-10-23 |
| 相关文档 | [10_Simulation](../modules/10_Simulation.md) · [07_WebApi](../modules/07_WebApi.md) · [Config_Schema §2.6/§13](../Config_Schema.md) · [ADR-0016](./0016-p2-boundary-consolidation.md)（§2.4 被本 ADR 取代） |
| 提案来源 | P2-4 将仿真显式限制为 `server.json` 单实例后，UI 仍按协议提供仿真参数编辑；实际需求是同时运行多个仿真设备 |

---

## 背景

ADR-0016 §2.4 规定「一份 `server.simulation` 只承载 0 号协议」，多协议仿真通过多实例部署实现。该决策解决了“静默取 0 号协议”，但带来两个问题：

1. 仿真实例无法与协议建立可配置的绑定：`server.json` 没有 `protocol` 字段，0 号协议由文件名字典序决定；
2. 前端 `SimPanel` 与 `/api/config/schema` 早已按协议暴露 `simulation` 段，用户设置后后端并不读取，形成“UI 可编辑、运行不生效”的假能力。

在单进程内同时模拟多个设备（例如 Modbus 主站 + 另一路 Modbus/S7 设备）是联调刚需，继续要求“多开进程 + 多份配置目录”不符合使用预期。

## 决策

### 2.1 协议级 simulation 为多实例主路径

- `ProtocolConfig` 增加 `SimulationConfig simulation`；`protocols/*.json` 顶层可写 `simulation` 段；
- `simulation.listenPort > 0` 即启用该协议自己的仿真实例：独立 `SimulationServer`、独立数据区、独立端口；
- 因此“增加仿真设备”= 在目标协议文件写/改 `simulation`，无需新增进程或复制配置目录；
- 一个协议最多一个实例；同一端口不得被两个实例占用，端口冲突在启动前拒绝。

### 2.2 server.json 单实例保留为旧配置回退

- 仅当**没有任何协议**声明 `simulation.listenPort > 0` 时，才使用 `ServerConfig.simulation`，并承载 0 号协议；
- 两者同时存在时协议级优先，启动日志明确提示 `server.json simulation` 被忽略；
- 旧配置（含 E2E 测试目录与 `configs_demo`）无需迁移即可继续运行。

### 2.3 热重载原子性

`ApplyRuntimeSync` 的校验顺序调整为：

1. 加载并构建新快照（不停止旧仿真器）；
2. 收集启用的仿真实例并检查端口冲突；
3. 校验通过后才停止旧实例、发布新快照、启动新实例。

端口冲突等配置错误返回 `ConfigError`，旧仿真器与旧快照保持运行，配合 `ConfigStore` 的自动回滚不会出现“半启动”状态。

### 2.4 API 与 UI 绑定键

- `/api/sim/status` 每个实例返回 `{protocol, listenPort, registerCount}`；
- `GET /api/sim/registers?proto=<protocolName>`、`POST /api/sim/registers` 的 `protocol` 字段均使用**协议名**；
- `WebUI` 的“仿真参数配置”继续写协议文件，保存后触发热重载，无需新增 `server` 配置域。

## 影响文件

| 层 | 文件 |
|----|------|
| Core | `Config.hpp`（`ProtocolConfig.simulation`）、`SimulationConfig.hpp`、`ServerConfig.hpp`（回退语义注释） |
| Service | `ConfigInternals.hpp`、`ProtocolJsonParser.cpp`（共用解析）、`SchemaChecker.cpp`（结构/取值校验）、`ConfigDirectoryLoader.cpp`（server.json 复用同一校验） |
| App | `RuntimeGlue.cpp`（多实例装配、端口冲突、旧配置回退） |
| 配置 | `configs/protocols/modbus-tcp.json`（仿真段迁入）、`configs/server.json`（回退关闭） |
| Simulation | `SimulationServer.cpp`（监听地址对齐文档：仅绑定 `127.0.0.1`，原实现为 `0.0.0.0`） |
| 测试 | `E2EMain.cpp` Test 16d（两个协议各一实例 + 回退端口未监听） |
| 文档 | `Config_Schema`、`modules/04/07/08/10`、`architecture/06`、本 ADR |

## 验证

- Linux/CMake 构建通过；`ctest` 5/5 通过；
- E2E 新增：两个协议分别监听 11533/11534，`server.json` 的 11532 不再监听，引擎经新端口读回初值 11；
- 既有单实例回退用例（Test 16 前半段）保持通过。

## 后果与约束

- 协议文件不再“纯语法”：`simulation` 是仿真实例的部署绑定；数据区仍是协议无关的寄存器阵列；
- 同一协议需要多个从站/数据区时，当前模型不支持（一个协议一个实例）；如需按 UnitID 路由，必须新增协议级设备模型，另行评审；
- 多实例端口必须是不同值；修改端口后旧端口立即释放，客户端需同步修改设备连接端口；
- `server.json` 的全局仿真段进入维护模式，不再扩展新字段。

## 被否决的方案

| 方案 | 否决原因 |
|------|----------|
| `server.json` 增加 `simulations[]` 数组 | 需要新增 `ConfigScope::Server` 与前端编辑器，改动面远大于收益；现有 UI/协议 schema 已按协议组织 |
| 保持单实例、要求多进程部署 | 无法满足单进程多设备联调；配置与端口管理成本转嫁给用户 |
| 单实例内按 UnitID 多从站 | UnitID 是 Modbus 专有语义，会把协议知识写进仿真器，违背“仿真器不硬编码协议”的边界 |

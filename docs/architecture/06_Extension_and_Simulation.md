# MyProtV2 ▸ 扩展指南与模拟仿真

> **所属**: MyProtV2 架构设计系列
> **上一篇**: [可观测性、配置与构建](./05_Observability_Config_Build.md)

---

## 十五、扩展指南

### 15.1 新增协议

纯配置，无代码修改（字段规范见 [Config_Schema.md](../Config_Schema.md)）：

```json
{
    "protocolName": "OPCUA_Binary",
    "transport": { "type": "Tcp", "defaultPort": 4840 },
    "framing": {
        "type": "LengthField",
        "lengthFieldOffset": 4,
        "lengthFieldLength": 4,
        "lengthIncludesHeader": true,
        "byteOrder": "LittleEndian",
        "maxFrameSize": 65535
    },
    "operations": {
        "ReadValue": {
            "requestTemplate": ["...", "{Length:X4}", "..."],
            "responseParser": {
                "validCondition": "resp[0] == 0x00",
                "dataStartIndex": 8
            }
        }
    },
    "builtInFunctions": ["auto"]
}
```

> **v1 边界**：RequestBuilder 为**单遍展开**，长度等字段由标签变量直接给出（如 Modbus TCP 用 `RegisterCount` 配合批量合并逻辑）；`{L:calc:Xn}` 二遍扫描未实装，模板出现即 `BuildError`（校验器规则 7，见 [02_Engine.md](../modules/02_Engine.md) RequestBuilder 一节"v1 边界"注记）。

### 15.2 新增模板片段

v1 的 RequestBuilder 为**单遍展开的具体类**，无 `IFunctionProvider` 扩展点（早期接口体系已随编排器一并删除，见 [02_Engine.md](../modules/02_Engine.md) v4 注记）。实装片段：

| 片段 | 语义 | 实现 |
|------|------|------|
| `{Name:Xn}` | 变量定宽十六进制输出 | RequestBuilder |
| `{Name:raw}` | 变长字节流直插（载荷由写请求运行时注入） | RequestBuilder::BuildBytes 重载（见 ADR-0007） |
| `{Name:auto:Xn}` | 原子自增计数（按宽度取模） | AutoIncrementProvider |
| `0A1B` | 十六进制字面量 | RequestBuilder |

新增片段（如 BCD 编码、协议专有编码）属 **Engine 核心变更**（见 [ROADMAP.md](../ROADMAP.md)），不在协议层局部引入，统一走 ROADMAP 评估；协议特有计算（如 Modbus PDU 长度）用**注册式策略**在 App 层解决（[PDULengthRegistry](../modules/05_Gateway.md)，启动时按协议注册，Engine 模板不感知协议知识）。

### 15.3 新增通道类型

```cpp
// SerialChannel 已纳入范围 (见 ADR-0002); CAN / UDP 等其他通道见 ROADMAP.md
class UdpChannel : public Transport::IChannel { /* asio socket + 回调式三件套 */ };
```

### 15.4 协议/配置持久化扩展

无 `IProtocolRepository` 仓库抽象——协议 / 设备 / 标签统一由 `ConfigStore`（JSON 文件 + 管理 API）承载，运行期经 `ProtocolLookup` 回调注入 Gateway。SQLite / 分布式配置中心等持久化后端属远期候选，见 [ROADMAP.md](../ROADMAP.md)；届时替换的是 `ConfigStore` 的存储底座与 `ProtocolLookup` 的数据源，模块间接口不变。

---

## 十六、模拟仿真层 (Simulation)

> **v1 实态为"黑盒仿真"**：独立 TCP 服务端模拟设备，网关经真实 `IChannel` 连接——不采用进程内 `IChannel` 替身。
> 详细设计见 [Simulation 模块设计](../modules/10_Simulation.md)。

### 16.1 v1 实态：配置驱动 SimulationServer

| 组件 | 职责 |
|------|------|
| `SimulationServer` | 每个仿真设备一个实例；自有 `io_context` + 专用工作线程，全异步 accept/read；监听 `ServerConfig.simulation` 指定端口 |
| `TemplateMatcher` | 按协议 `OperationConfig.requestTemplate` 渲染出的字节模式匹配入站请求 |
| `ResponseSynthesizer` | 依响应模板合成应答字节 |
| `SimulationDataStore` | `/api/sim` 寄存器读写的寄存器数据底座 |

- **装配**：App 层 `ApplyRuntimeSync` 按设备配置批量创建 / 重建仿真器（首次启动与热重载同一入口）
- **操控**：WebApi `/api/sim/*`（status 清单 / registers 读 GET 写 POST），handler 在 `App/RuntimeGlue`
- **价值**：E2E 全链路（真实 TCP → ChannelManager → 轮询 → 解析）无硬件验证

### 16.2 搁置的进程内仿真设计

以下能力**有完整设计稿但未实装**，启用评估见 [ROADMAP.md](../ROADMAP.md)：

| 能力 | 状态 | 设计稿归档 |
|------|------|------------|
| `DeviceSimulator` / `ISimChannel`（进程内通道替身） | 搁置 | [10_Simulation §10.11](../modules/10_Simulation.md) |
| `PlaybackEngine` / `Recorder`（流量录制回放） | 搁置 | 同上 |
| `FaultInjector`（故障注入装饰器） | 搁置 | 同上 |

---

## 十七、扩展与搁置项索引

尚未实装的扩展候选统一登记在 [ROADMAP.md](../ROADMAP.md)（含优先级、依赖、启用影响），本文件不重复列表。

---

> **文档版本**: v4.0（v3.1 基础上实态化：§15.1 示例去除未实装的 calc 占位；§15.2 去除 IFunctionProvider 扩展示例，改为实装片段表；§15.4 去除 IProtocolRepository，改 ConfigStore/ProtocolLookup 实态；§16 由进程内 ISimChannel 设计稿改写为 SimulationServer 黑盒仿真实态 + 搁置项索引）  
> **上一篇**: [可观测性、配置与构建](./05_Observability_Config_Build.md)  
> **相关文档**: [Simulation 模块详细设计](../modules/10_Simulation.md)

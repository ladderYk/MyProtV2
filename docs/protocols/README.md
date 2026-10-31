# 协议教学文档

> 本目录面向**已掌握协议规范、想了解 MyProt 配置如何映射到协议字节**的读者。
>
> 主文档 [docs/README.md](../README.md) 介绍 MyProt 整体架构与文档维护原则；本目录专注"配置 → 字节"对照。

## 阅读路径

1. 已有 Modbus 基础 → 直接看 [modbus-tcp.md](./modbus-tcp.md)
2. 接西门子 S7-1200/PLC → 看 [s7-1200.md](./s7-1200.md)
3. 接仙工（SEER）AGV 控制器 → 看 [seer.md](./seer.md)
4. 接欧姆龙 NJ/NX、CJ/CS（FINS/TCP）→ 看 [omron-fins-tcp.md](./omron-fins-tcp.md)
5. 接三菱 Q/L/iQ-R（MC 3E 帧）→ 看 [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md)
6. 接倍福 TwinCAT（ADS/AMS）→ 看 [twincat-ads.md](./twincat-ads.md)
7. **只想先跑通一份最小协议** → 看下节「5 分钟最小协议」，用 `--proto-init` 生成骨架
8. 想加新协议 → 看本 README 的"如何新增一个协议"段
9. 想理解配置 schema → 回 [Config_Schema.md](../Config_Schema.md)

## 5 分钟最小协议（`--proto-init`）

不想从零手写 JSON 时，用内置脚手架生成一份**能通过加载期校验**的最小协议骨架：

```bash
# 列出可选模板（不带 kind 时打印三种）
MyProt.App.exe --proto-init

# 生成 configs/protocols/my-lengthfield-proto.json
MyProt.App.exe --proto-init lengthfield

# 写到别处 / 覆盖已有文件
MyProt.App.exe --proto-init fixed --out configs/protocols/plc-a.json --force
```

| 模板 | 成帧 | 适用场景 |
|------|------|----------|
| `lengthfield` | `LengthField`（长度域 + 头长） | 有长度前缀的二进制帧（Modbus 类），**推荐起点** |
| `fixed` | `Fixed`（定长） | 每帧字节数固定的小型指令协议 |
| `serial` | `Silence`（串口静默分帧） | RS-485/RTU 风格，按字符时间 + 帧间静默切帧 |

生成后三步接入：

1. **改字节布局**：把 `operations.<op>.requestTemplate` 换成设备手册的帧；`responseParser.validCondition / dataStartIndex / dataLengthExpr` 按响应结构改。占位符写法见下节。
2. **加设备**：`configs/tags.json` 的 `devices[]` 加一条，`protocol` 填模板里的 `protocolName`，`connection.host/port` 填设备地址（串口填 `portName`）。
3. **加标签**：`tags[]` 加一条，`operation` = 模板里的 `ReadRegisters`，`variables` 写 `StartByteAddress` / `ByteCount`（引擎统一用**跨协议字节单位**，`StartAddress` / `RegisterCount` 由协议 `outputs` 派生）。

同型号设备只差站号/节点号时**不要复制协议文件**：把差异写进 `devices[].variables`（如 `"UnitID": 3`），标签同名键仍可覆盖，见 [Config_Schema.md §3.4](../Config_Schema.md#34-变量就近覆盖与继承)。

`--proto-init` 只写文件，不加载配置也不启动网关；`--out` 的父目录会自动创建，目标已存在时须显式加 `--force`。生成物随后由 `MyProt.App.exe --config configs` 正常加载，模板本身在 `Service.Tests` 中逐份走真实校验器。

配置文本允许 `//` 行注释与 `/* ... */` 块注释（解析前丢弃，不改变语义），可以把手册页码、字节含义直接写在字段旁：

```jsonc
{
  // 帧头: 事务号(2) + 长度域(2)
  "requestTemplate": [ "00 01", "{Len:X2}", "{UnitID:X2}" ],
  /* 站号按设备改；同型号设备请用 devices[].variables 覆盖，不要复制本文件 */
  "inputs": { "UnitID": { "source": "static", "value": 1 } }
}
```

字符串字面量里的 `//` / `/*` 原样保留；注释只在“文本 → 对象”这一步丢弃，热重载、渲染与 `--validate` 的行为与无注释配置完全一致。注意 WebUI 的**表单模式保存会重新序列化并丢失注释**，需要长期保留时请用 JSON 模式编辑。

## 写完先校验（`--validate`）

改完 JSON 不必启动网关，直接跑与启动期同一套校验器：

```bash
# 整个配置目录: protocols/*.json + tags.json (+ server.json 复检)
MyProt.App.exe --validate configs

# 单个协议文件 / 单个 tags.json
MyProt.App.exe --validate configs/protocols/plc-a.json

# 机器可读输出 (供 CI / 脚本消费)
MyProt.App.exe --validate configs --json
```

每条问题带 RFC 6901 JSON Pointer（如 `/operations/Read/outputs/AddrHi`），可直接被编辑器或 CI 定位；退出码 `0` = 通过（警告不阻塞）、`1` = 有错误、`2` = 用法或 IO 错误。目录模式逐文件校验、一次列全；全部通过后再用真实加载器复检 `server.json`、仿真模板等只有加载期才覆盖的检查——**这里通过 ≈ 网关能启动**。

## 试渲染一帧（`/api/preview`）

模板改完不确定“发出去到底是什么字节”时，不必启动网关：

- **WebUI**：协议配置页工具栏 **「试渲染」**（用当前编辑器内容，含未保存修改与注释）
- **命令行**：

```bash
curl -X POST "http://127.0.0.1:8080/api/preview?scope=protocols&name=plc-a" \
  -H "Content-Type: application/json" \
  -d '{"operation":"Read","variables":{"StartByteAddress":100,"ByteCount":2}}'
```

返回 `hex` / `ascii` / `frameLength` / `variables[]`（含每个变量的来源层）/ `lengthField`（槽位值、期望值与是否一致）。渲染走与网关**同一条** `RequestBuilder` 管线（含 `derivedLength` 注入与 `autoCompute`），且**只读**：不落盘、不重载、不连设备。省略 `protocol` 时读磁盘现值；传 `protocol`（JSON 文本或对象）即可预览未保存的候选内容。

反向问题——“设备返回这串字节，协议解得对吗？”——用同一个面板的 **「响应解析」** 页签：

```bash
curl -X POST "http://127.0.0.1:8080/api/preview/response?scope=protocols&name=plc-a" \
  -H "Content-Type: application/json" \
  -d '{"operation":"Read","hex":"03 04 00 2A 00 01","finalType":"UInt16","byteCount":2}'
```

返回 `ok` / `condition{checked,expr,ok}` / `dataStartIndex` / `byteOrder` / `frameLength` / `rawHex` / `value{type,text,numeric}` / `quality`。`validCondition` 不满足、数据不足或 hex 非法时返回 `ok:false` 并在 `errors[]` 说明原因；`byteCount` 超出数据区时为剩余全量并给出警告。解析器与网关（`Engine::ResponseParser`）为同一个。POST 时 `protocol` 可覆盖磁盘内容；额外传 `converters`（与 tags.json 同形的 `{kind:"scale",k,b}` 链）即可预览标签的读后换算，响应回传 `valueBeforeConversion` 与实际换算链。

## 已支持协议

| 协议 | 配置 | 教学文档 | 状态 |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./modbus-tcp.md) | ✅ v1 |
| Siemens S7-1200 (S7comm/ISO-on-TCP) | [configs/protocols/s7-1200.json](../../configs/protocols/s7-1200.json) | [s7-1200.md](./s7-1200.md) | ✅ v1（仅 Read/Write Var；无内置仿真器）|
| 仙工智能 SEER AGV（0x5A 01 + JSON 正文）| [configs/protocols/seer.json](../../configs/protocols/seer.json) | [seer.md](./seer.md) | ✅ v1（正文需 hex 编码；无内置仿真器）|
| Omron FINS/TCP（`FINS` 魔数 + 大端长度域）| [configs/protocols/omron-fins-tcp.json](../../configs/protocols/omron-fins-tcp.json) | [omron-fins-tcp.md](./omron-fins-tcp.md) | ✅ v1（WR 区字读；无内置仿真器）|
| Mitsubishi MC 3E（`50 00` 子头 + 小端长度域）| [configs/protocols/mitsubishi-mc-3e.json](../../configs/protocols/mitsubishi-mc-3e.json) | [mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md) | ✅ v1（D 区字读；无内置仿真器）|
| Beckhoff TwinCAT ADS/AMS（6 字节信封 + 全小端）| [configs/protocols/twincat-ads.json](../../configs/protocols/twincat-ads.json) | [twincat-ads.md](./twincat-ads.md) | ✅ v1（ADS Read；⚠️ NetId / IndexGroup 需按现场工程改）|

## 如何新增一个协议（指引）

> 完整步骤不写在这里——见 [ROADMAP.md §3 评审流程](../ROADMAP.md) 与 [adr/](../adr/) 现有决策。

最小步骤：

1. **写协议 JSON**：`configs/protocols/<name>.json`
   - 优先用 `MyProt.App.exe --proto-init lengthfield` 生成骨架，再按手册替换字节（字段清单见 [Config_Schema.md §3](../Config_Schema.md)）
   - 根级必填 `protocolName` / `schemaVersion` / `transport` / `framing` / `operations`；每个 op 必须有 `requestTemplate`；`responseParser` 的三个字段可由操作自身声明，也可（引用 `profile` 时）来自协议级 `operationProfiles` 的展开结果——两者合并后必须齐备
   - 同协议不同设备只差常量时用 `devices[].variables`，不必复制协议文件
2. **写教学文档**：`docs/protocols/<name>.md`
   - 帧结构表 + 配置↔字节映射 + 异常处理
3. **注册协议插件**（仅当 PDU 长度需运行时计算）：见 [modules/05_Gateway.md §5.3](../modules/05_Gateway.md#53-写路径)
4. **加 E2E 测试**（可选但推荐）：仿真器 → MyProt → LatestValueStore → WebUI

## 协议模板通用约定

### 占位符语法（详见 [Config_Schema.md §2.4](../Config_Schema.md#24-占位符模板)）

| 形式 | 含义 | 典型用途 |
|------|------|----------|
| `{Name:X4}` | 16-bit 大端无符号 | 寄存器值 |
| `{Name:raw}` | 原样 hex 字符串 | 变长写 |
| `{TransactionID:auto:X4}` | 自动递增 16-bit | Modbus TCP TransactionID |
| `{PDULength:X2}` | PDU 长度（注册式策略） | Modbus FC15/FC16 长度字段 |

### responseParser 通用约定

- `validCondition`：判响应合法性的 Mini-C 表达式；`resp[N]` 表示 N 字节偏移
- `dataStartIndex`：数据区起点偏移
- `dataLengthExpr`：数据区长度计算式
- 多个操作共用同一套解析规则时，把它们收进协议级 `operationProfiles`（支持 `extends` 单继承），操作侧只写差异字段并用 `profile` 引用；展开语义与逐操作明写完全等价，见 [Config_Schema.md §2.7](../Config_Schema.md) 与 [ADR-0022](../adr/0022-operation-profiles.md)

## 异常处理通用约定

- **MyProt 端的"异常响应"语义** = `validCondition` 不通过
- 不会自动解析 Modbus 异常码（0x01~0x0B），**只判 valid**
- 需要细化异常处理时，可在 `validCondition` 里加 `|| resp[7] == 0x83` 等判异常分支

## 排错通用路径

```
[Data] quality=Bad
   ↓
看 [Channel] 标签: 连接/超时/RST
   ↓
看 [Data] 后续: responseParser 是否报"条件不通过"
   ↓
仿真器侧看 raw 响应: 用 Wireshark 过滤 tcp.port == <protocol-port>
   ↓
对照本目录教学文档的"帧结构表"查错
```

## 关联索引

- 主架构: [02_Layered_Architecture.md](../architecture/02_Layered_Architecture.md)
- 配置规范: [Config_Schema.md](../Config_Schema.md)
- 扩展候选: [ROADMAP.md](../ROADMAP.md)

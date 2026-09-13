# ADR-0007：写路径范围与形态

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** |
| 相关文档 | [Config_Schema.md](../Config_Schema.md) · [modules/07_WebApi.md](../modules/07_WebApi.md)（`/write` 端点）· [modules/01_Core.md](../modules/01_Core.md)（`WriteTimeout`/`WriteFailed`/`FormatSpec::Raw`）· [ADR-0003](./0003-known-issues.md) |

---

## 背景

写路径（write）的难点不在"能不能把请求发出去"，而在于：

- 占位符 `{Name:raw}`（变长字节注入）如何渲染进模板？
- 写请求的响应如何校验？
- 要不要写后读回验证（read-back）？
- 无响应写协议（某些单向 RTU 写）如何处理？
- 写失败如何回滚 / 告警？

这是一道**端到端数据流**问题——写非幂等（写错寄存器会改变物理设备状态），因此不能简单当成"读请求的特例"。

## 决策驱动因素

- **读路径已闭环**：模板 → 发送 → 成帧 → 解析 → 类型转换 → 质量码 → 分发。写路径可复用同一条管线的构建与传输能力，无需独立数据流。
- **写非幂等，风险高于读**：写错寄存器可能改变物理设备状态 → 写必须**恒单次尝试、不重试**，且**需要写后读回**才敢认定值已落地。
- **复杂度集中在两点**：写响应校验语义（echo 而非解析）与并发写互斥——均可通过"echo 校验 + 串行化"控制。
- **契约要诚实**：写成功的判定标准必须明确（echo OK ≠ 值已落地），read-back 是可选但必要的第二道闸门。

## 备选方案

### 方案 A：实现完整写路径（含写后读回、无响应写、并发写互斥）

- 优点：功能完整，与读共用模板管线。
- 缺点：验证面显著大于读；写非幂等带来安全与测试负担；无响应写需协议级补充设计。

### 方案 B：只做读，写路径整体预留

- 优点：聚焦读采集核心价值；范围边界诚实（`/write` 明确返回 501）。
- 缺点：无法满足写需求。

## 决策

**采用方案 A：写路径纳入范围，与读共用同一套模板管线**。

理由：模板引擎（`RequestBuilder`）与错误码（`WriteTimeout`/`WriteFailed`）已就位，"占位不实现"反而制造半悬空状态；写路径的实际复杂度集中在**响应校验语义**与**并发**两点，均可通过"echo 校验 + 串行化"控制，无需独立数据流设计。

### 1. 范围边界

写路径**纳入范围**，与读共用同一套模板管线：

| 项 | 形态 |
|---|---|
| 引擎 | `TagReader::WriteOnce(tag, protocol, writeOperation, value, channel, timeout, handler)`（标量）/ `WriteBytes`（变长） |
| 入口 | `POST /api/data/write` —— 成功 200、入参非法 400、网关失败 502 |
| 标量渲染 | 定宽占位符 `{WriteValue:Xn}`；`Float`/`Double` 走 `{WriteValue:raw}` |
| 变长渲染 | `{Name:raw}` 消费运行时注入的载荷字节流（见下文） |
| 响应校验 | 复用 `responseParser.validCondition` 做 echo 校验，不解析数据区；失败回 `WriteFailed` |
| 错误码 | `WriteFailed`（echo 校验失败）；`WriteTimeout` 未单独使用——超时统一走通用 `Timeout` |

### 2. 形态契约

- 占位符 `{Name:raw}`：从写请求注入的字节数组变量渲染。
- 错误码 `WriteTimeout` / `WriteFailed`：不可重试（写非幂等；`IsRetryable` 见 [ADR-0006](./0006-quality-semantics.md)，映射为 `Bad`）。
- `FormatSpec::Raw`：写路径格式标记。
- 写操作复用 `OperationConfig`（`requestTemplate` + `responseParser`），不另设结构。

### 3. 关键决策

| 决策 | 理由 |
|------|------|
| 写响应只做 echo 校验，不解析数据区 | 写应答的语义就是"设备回显请求"，解析无意义 |
| 恒单次尝试、不重试 | 写非幂等，重试可能重复写入 |
| 超时 = `device.requestTimeoutMs` | 与读路径统一，避免两处超时漂移（[ADR-0004](./0004-timeout-retry-budget.md) R1） |
| 并发以串行化替代显式锁 | 写链整体 post 到引擎 io 线程，与轮询回调串行执行 |
| 鉴权 + 限流 | token 鉴权 + 令牌桶（429），衔接 [ADR-0008](./0008-management-plane-security.md) |

**配置代际**：写路径不引入必填字段，不触发 `schemaVersion` 递增（[ADR-0005](./0005-config-versioning.md)）。

### 4. 写后读回验证（read-back）

`POST /api/data/write` body 可带 `"readBack": true`（默认 false）：写应答 echo 校验通过后立即对同地址发起读，不一致回调 `Error::Code::ReadBackMismatch` → HTTP 503。详见 [modules/07_WebApi.md §7.5](../modules/07_WebApi.md)。

**无响应写（单向 RTU）** 尚未设计，见 [ROADMAP.md](../ROADMAP.md)。

## `{Name:raw}` 变长字节渲染

以**两类载荷表**为核心：`variables: map<string,uint32>`（定宽标量）与运行期 raw 载荷表（hex 字符串 → 字节流）供 `{Name:raw}` 消费。二者在同一 `requestTemplate` 内并存，由 RequestBuilder 按占位符第二段路由（定宽走 `Build`、raw 走 `BuildBytes`）。

**端到端链路**（变长写，与标量写并列）：

```
POST /api/data/write  body: {"tag":"PLC-001.MultiReg","bytes":"01 0A 0B 0C"}
  → WebApi 解析 bytes 字段 (hex 字符串 → vector<uint8_t>)
  → io.post 全链 (按值拷贝 tag/protocol/bytes)
  → ChannelManager.GetOrCreateChannel
  → TagReader.WriteBytes(tag, protocol, "WriteMultipleRegisters", rawVars={Payload: hex})
      → RequestBuilder.BuildBytes(op, variables, variableBytesHex)
          → {Payload:raw} 命中 → 解析 hex 字符串为字节流 → 直插请求帧
      → SendReceive(device.requestTimeoutMs)
      → echo validCondition 校验
  → 200 {"ok":true}
  → 400 互斥 (value+bytes 同发 / 都未发 / bytes 非 hex)
  → 502 错误码同上
```

**协议侧新增约定**（仍为 `operations` 成员，不新增 schema 字段）：

- **写操作名 `WriteMultipleRegisters`**（沿用 Modbus FC16 习惯）——协议作者在 `operations` 中声明这个名，`requestTemplate` 形如：

  ```jsonc
  "WriteMultipleRegisters": {
    "requestTemplate": [
      "{TransactionID:auto:X4}", "00 00", "{Length:calc:X4}",
      "01", "10", "{StartAddress:X4}", "{RegisterCount:X4}", "{ByteCount:X2}",
      "{Payload:raw}"  // ← 变长载荷 (P1 A)
    ],
    "responseParser": { "validCondition": "resp[7]==0x10" }
  }
  ```

- `{Payload:raw}` 的字节流由**写请求运行时注入**（v1.32 定稿）：载荷来自 `POST /api/data/write` 的 `bytes` 字段，或以 `writeVariable`（默认 `WriteValue`）为键的编码值；引擎以该键注入运行期 raw 表，`{Name:raw}` 查表直插。
- 启动期规则 14：模板里出现 `{Name:raw}` 时，该名须为运行时注入的写值变量（`writeVariable`），否则报"变长模板变量未定义"。

> **v1.32 修订（配置字段移除）**：原先设计的 Config 字段 `DeviceConfig.variableBytesHex` / `TagDefinition.variableBytesHex` 已**删除**。运行时核实结论：这两个字段的声明值**从不进入请求帧**——载荷恒由写请求注入（键 = `writeVariable`），字段只被校验器读取（旧规则 14 要求 raw 名出现在其中），构成"声明了也不生效"的陷阱。固定字节块可直接用模板 hex 字面量（`"00 00"` 行）表达，无需该字段。

**API**（与 `WriteOnce` 平行，对既有标量写路径无破坏）：

- `Engine::RequestBuilder::BuildBytes` — 接受运行期 raw 载荷表的 `Build` 重载；纯标量模板仍走 `Build` 入口
- `Gateway::TagReader::WriteBytes` — 接受运行期 raw 载荷表的 `WriteOnce` 重载；同样只在 bytes 路径被调用
- `App::HandleWriteApi` — `body` 增 `bytes` 字段（与 `value` 互斥）

**WebApi 契约**（修订 [modules/07_WebApi.md §7.5](../modules/07_WebApi.md)）：

| body | 路径 | 响应 |
|------|------|------|
| `{"tag":"X","value":N}` | `WriteOnce` → `WriteSingleRegister` | 200/400/502 同前 |
| `{"tag":"X","bytes":"01 0A 0B"}` | `WriteBytes` → `WriteMultipleRegisters` | 200/400/502 同前 |
| `{"tag":"X","value":N,"bytes":"..."}` | 互斥校验失败 | 400 |
| `{"tag":"X"}` (两者皆无) | 缺字段 | 400 |

**`bytes` 字段解析规则**（P1 A 简化版）：
- 接受 hex 字符串（"01 0A 0B" / "010A0B" / "01 0a 0b" 都可；空格/制表符忽略）
- 长度必须为偶数位十六进制，否则 400
- **base64 暂不实现**——保留 P1+ 扩展点；hex 形式足够覆盖 Modbus/S7/IEC104 全部目标场景

**配置代际**：本实装**不递增 `schemaVersion`**（ADR-0005）——`variableBytesHex` 为可选新字段，老配置无此字段仍可正确加载（变长写路径只在新字段出现时被路由）。

**验证**（E2E Test 14，详见 `src/Tests/E2EMain.cpp`）：
- `POST /api/data/write` 标量 + 变长两条路径同跑
- 仿真器对 `kind:"write"` 的操作仅做 echo，**不解析 FC06/FC16 数据区**——本实装仅验证"请求帧能正确组装 + echo 校验通过"，不验证"对端是否真的把数据写到目标寄存器"

## read-back



`{Name:raw}` 实装后，写应答 echo 校验只能确认从站"收到"请求，但**不能确认值实际落地**——部分从站会拒收某些字节、寄存器位宽截断、字节序错位等场景下 echo OK 但设备状态未变。read-back 闭环校验写入值与设备实际状态一致，是写路径的最后一道闸门。

**实现要点**：
- **错误码**：`MyProt::Core::Error::Code::ReadBackMismatch`（不可重试，避免重复写入；与 `WriteFailed` 区分）→ HTTP **503**。
- **TagReader 接口**：[`WriteOnce`/`WriteBytes`](../Gateway/include/MyProt/Gateway/TagReader.hpp) 增 `bool readBack` 参数；实现层在 echo 校验通过后立即对同地址发起读。
- **协议契约**：
  - 标量 + readBack：用协议里的 `ReadHoldingRegisters` 操作模板构建读请求（注入 `RegisterCount=1`）；取 Modbus FC03 PDU 头 8 字节 + 字节计数 1 字节 + 寄存器值 2 字节布局的 `[9..10]` 字节按 big-endian 解析 16-bit，与 `value` 比对。
  - 变长 + readBack：用**写操作名**（如 `WriteMultipleRegisters`）的模板构建读请求并注入 `RegisterCount = totalBytes / 2`——此为简化遗留，**应改为用读操作模板**，见 [ADR-0003 变长 read-back](./0003-known-issues.md#变长-read-back)。
  - 协议无 `ReadHoldingRegisters` 定义时**静默跳过** read-back（视为协议不支持，直接回调成功，不退化为 503）。
- **资源复用**：同一 `IChannel` 同一 `framing`；读回超时复用 `requestTimeoutMs`，不复用读轮询 `pollIntervalMs`。
- **HTTP 端点**：`POST /api/data/write` body 增 `"readBack": true`（默认 false；可与 `value` 或 `bytes` 任意组合）。错误码 503 在错误码表中新增一行。
- **简化原则**：**API 层 only**——不引入 device 级 readBackPolicy 配置（enabled / timeoutMs / tolerance），见 [ROADMAP.md](../ROADMAP.md)。

## 端到端链路

> 写路径与读网关共用同一套模板管线：写非幂等 → 恒单次尝试；响应只做 echo 校验。

**端到端链路**（`src/App/RuntimeGlue.cpp` `WriteViaGateway` / `HandleWriteApi`）：

```
POST /api/data/write  body: {"tag":"PLC-001.SetPoint","value":123}
  → WebApi 线程 io.post 全链（查 tag → device → protocol，均按值拷贝防热重载悬垂）
  → ChannelManager.GetOrCreateChannel（首连异步 / 已连 fast-path）
  → TagReader.WriteOnce：
      → 按约定操作名 "WriteSingleRegister" 查 protocol.operations（未声明即 ConfigError）
      → 变量表 = tag.variables + {StartAddress: 标签地址} + {WriteValue: value}
      → RequestBuilder 构建请求 → SendReceive(device.requestTimeoutMs)
      → 应答仅做 validCondition echo 校验，不解析数据区
  → 200 {"ok":true}
  → 400 入参非法（JSON 解析失败 / 字段缺失 / value 不在 [0,65535]）
  → 502 {"error":"..."}（TagNotFound / DeviceNotFound / ProtocolNotFound /
     ConfigError / WriteFailed / Timeout 等）
```

**关键契约**：

- **协议侧零新增 schema 字段**：写操作就是普通 `operations` 成员（约定名 `WriteSingleRegister`），`requestTemplate` 用定宽占位符、`responseParser.validCondition` 做 echo 校验——与读操作结构完全一致。
- **启动期校验天然放行**：规则 14（Config_Schema §7）只校验 tag 引用的操作；写操作不被任何 tag 引用，故其运行期注入变量（`WriteValue` 等）不会在加载期报"模板变量未定义"。
- **线程模型**：整条写链 post 到引擎 io 线程，与轮询回调串行——消除跨线程 socket 操作与配置向量竞争；WebApi 侧以 shared_ptr 保活与 promise + 10s `wait_for` 兜底，防止 io 卡死拖挂管理面。
- **验证**：E2E Test 13 覆盖全链路（FC06 写入仿真器内存并回读核对，见 `src/Tests/E2EMain.cpp`）；仿真侧由 `configs_write_test/protocols/simmodbusw.json` 提供 `kind:"write"` 的 simulation 操作。

## 影响

- `Config_Schema.md` §3.2/§4/§5/§9：写路径字段（`writeOperation` / `writeBytesOperation` / `writeVariable`）与写操作约定。
- `modules/07_WebApi.md`：`/write` 端点为 `POST /api/data/write`（标量 `value` / 变长 `bytes` / `readBack`）。
- `modules/01_Core.md`：`WriteTimeout` / `WriteFailed` / `FormatSpec::Raw` 为写路径正式错误码与格式标记。

## 复核触发条件

- 写路径剩余未做项（无响应写、显式并发写互斥、变长 read-back 读操作模板）见 [ADR-0003 未决问题登记](./0003-known-issues.md)。

---

> **相关文档**: [Config_Schema.md](../Config_Schema.md) · [modules/07_WebApi.md](../modules/07_WebApi.md) · [ADR-0003](./0003-known-issues.md) · [ADR-0006](./0006-quality-semantics.md) · [文档索引](../README.md)

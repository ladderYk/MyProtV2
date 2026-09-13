# Modbus TCP 协议教学

> 本文面向**已掌握 Modbus TCP 协议规范、想了解 MyProt 配置如何映射到协议字节**的读者。
>
> **前置知识**：Modbus TCP 协议帧结构（MBAP + PDU）、功能码（FC）概念。
>
> **关联文件**：
> - 配置：[configs/protocols/modbus-tcp.json](../../configs/protocols/modbus-tcp.json)
> - 规范：[Modbus Application Protocol V1.1b3 (Modbus Org 2012)](https://modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
> - 配置 Schema：[docs/Config_Schema.md](../Config_Schema.md)

---

## 0. 30 秒速览

```json
// modbus-tcp.json 顶层 6 个字段
{
  "transport":         "Tcp:502",                    // 1. 传输层
  "framing":           "LengthField at offset 4",    // 2. 帧定界
  "inputs":            "UnitID 静态值 / TransactionID 自增", // 3. 协议级输入段
  "outputs":           "PDULength 等派生长度",       // 4. 协议级输出段 (derivedLength)
  "operations":        "8 个 FC: 01/02/03/04/05/06/0F/10", // 5. 8 个功能码
  "handshake":         []                            // 6. 无握手
}
```

**MyProt 的协议配置 = 协议规范的"投影"**：
- **framing** 对应 MBAP 的 Length 字段定位
- **operations** 对应每个 FC
- **每个 op 的 requestTemplate** 对应 PDU 字节序列
- **每个 op 的 responseParser** 对应响应解析规则

---

## 1. Modbus TCP 帧结构回顾（MyProt 视角）

### 1.1 请求帧（MBAP + PDU）

```
偏移: 0  1  2  3  4  5  6  7  8  9  ...
内容: [TransactionID---][ProtocolID][Length-----][UnitID][Func][Data...]
字节:   2               2          2             1      1     N
```

| 偏移 | 字段 | 长度 | 取值 | 含义 |
|:-:|------|:--:|------|------|
| 0..1 | TransactionID | 2 | auto (MyProt 自增) | 事务 ID，**Modbus TCP 用** |
| 2..3 | ProtocolID | 2 | 0x0000 | 协议标识（0=Modbus 协议） |
| 4..5 | Length | 2 | PDU 长度（含 UnitID）| 后续字节数 |
| 6    | UnitID | 1 | 1~247 | 从站地址 |
| 7    | Func | 1 | FC | 功能码 |
| 8+   | Data | N | 依 FC | 业务数据 |

### 1.2 响应帧

```
偏移: 0  1  2  3  4  5  6  7  8  9  ...
内容: [TransactionID---][ProtocolID][Length-----][UnitID][Func][ByteCount][Data...]
```

**与请求帧的区别**：
- `Func` 字段：对**正常响应** = 请求 FC；对**异常响应** = 请求 FC + 0x80
- `ByteCount` 字段：仅读多路操作（FC01/02/03/04）有；1 字节
- `Data` 字段：内容与请求不同

### 1.3 异常响应（MyProt 视角）

```
偏移: 0  1  2  3  4  5  6  7  8
内容: [TransID--][ProtID][Length][UnitID][0xFunc|0x80][ExceptionCode]
```

| ExceptionCode | 含义 |
|:--:|------|
| 0x01 | Illegal Function（FC 不支持）|
| 0x02 | Illegal Data Address（地址越界）|
| 0x03 | Illegal Data Value（值非法）|
| 0x04 | Slave Device Failure（设备故障）|
| 0x05 | Acknowledge（长命令，已接受）|
| 0x06 | Slave Device Busy（设备忙）|
| 0x08 | Memory Parity Error（存储校验）|
| 0x0A | Gateway Path Unavailable（网关路径不可用）|
| 0x0B | Gateway Target No Response（网关目标无响应）|

**MyProt 行为**：`validCondition` 不通过 → `quality=Bad` + `lastError.code=InvalidResponse`。**不**自动解析 ExceptionCode 含义。

---

## 2. 协议配置逐字段对照

### 2.1 transport

```json
"transport": {
  "type": "Tcp"         // 传输层类型; 设备级 connection.port 显式声明, 协议级 defaultPort 已弃用
}
```

- `type`: 必须是 `"Tcp"`（Modbus TCP 用 TCP）
- 设备级 `host:port` 在 [tags.json](../../configs/tags.json) 的 `connection` 段配置

### 2.2 framing（**最关键**）

```json
"framing": {
  "type": "LengthField",
  "lengthFieldOffset": 4,    // Length 字段起始偏移
  "lengthFieldLength": 2,    // Length 字段占 2 字节
  "lengthIncludesHeader": false,  // Length 不含 MBAP 头
  "byteOrder": "BigEndian",  // Modbus 网络字节序
  "headerLength": 6,         // 保留前 6 字节 (TransID+ProtID+Length)
  "lengthAdjustment": 0,     // 长度调整 (0 = 不调整)
  "maxFrameSize": 260        // Modbus TCP 最大 PDU 253 字节 + 7 字节头
}
```

**MyProt 怎么用 framing 解析响应**：

1. 接收字节流 → **跳过 6 字节头部**（MBAP）
2. 在 `lengthFieldOffset=4` 处读 `lengthFieldLength=2` 字节 → 得 Length 值 N
3. 后续 N 字节（含 UnitID+Func+Data）= PDU
4. 总帧长 = 6 + N = 6 + (UnitID(1) + Func(1) + Data) = 6 + 1 + 1 + Data = 8 + Data

**验证**：
- 读 1 个保持寄存器 (FC03) → Data=2 → 总帧 = 6 + (1+1+1+2) = 11 字节 ✅
- 读 10 个保持寄存器 (FC03) → Data=20 → 总帧 = 6 + 23 = 29 字节 ✅

### 2.3 inputs（协议级输入段）

取代旧单段 `variables` / `defaultVariables`：声明操作者提供或运行时生成的**输入**。

```json
"inputs": {
  "UnitID":        { "source": "static", "value": 1,
                     "label": "从站地址", "unit": "",
                     "enum": [1,2,3,4,5,6,7,8,9,10] },
  "ProtocolID":    { "source": "static", "value": 0, "label": "协议标识" },
  "TransactionID": { "source": "auto", "strategy": "autoIncrement",
                     "params": { "seed": 1 }, "label": "事务ID" }
}
```

- **`source=static`**：`value` 有 → 运行时注入变量池（供模板 `{UnitID:X2}` 渲染）；`value` 无 → 仅 UI 展示提示（原 `hint`）。协议级固定值（如 `ProtocolID: 0`）在协议 `inputs` 声明 `value` 即可自动注入，标签无需重复配置。
- **`source=auto`**：`strategy=autoIncrement` 在参数层渲染前自增注入（如 `TransactionID`）；`frameSlice`/`expr`/`crc` 为帧感知节点。
- **优先级**：`tag.variables > op.inputs 静态 > protocol.inputs 静态`（同名就近覆盖）。
- 模板 `{TransactionID:X4}` 不写 `:auto:` 令牌，自增行为由上述 `inputs` 声明驱动。
- 派生输出（如 `PDULength`）不入本段，见下 §2.4 `outputs`。
- 详见 [Config_Schema.md §2 inputs/outputs](../Config_Schema.md)

### 2.4 outputs（协议级输出段）

派生输出 = 按载荷字节数计算的**长度/数量字段**，仅允许 `source=auto strategy=derivedLength`，以 `expr` 表达（用 `{name:len}` 引用模板变量的字节长度，`{Name:raw}` 载荷 = 实际字节数）：

```json
// 写操作 inputs 段（载荷名仅 UI 展示；载荷由模板 {WriteValue:raw} 自动识别）
"inputs": {
  "WriteValue": { "source": "static", "label": "写入载荷" }
},
"outputs": {
  "PDULength": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7",
                 "label": "PDU 长度（含 UnitID）" }
}
```

- `expr` 用 `{载荷名:len}` 引用载荷字节数（如 `{WriteValue:len} + 7`），也可引用 `inputs` 中已声明名（普通引用取配置值）。
- 变长写载荷由模板 `{Name:raw}` 占位符自动识别，载荷名声明在**使用它的写操作** `inputs` 段（`source=static` 无 `value`，仅作 UI 展示，可省略）。
- 同一协议域内 `inputs` 与 `outputs` 不得同名（校验 Error）；跨操作允许同名不同角色。
- 详见 [Config_Schema.md §3.2.1](../Config_Schema.md)

### 2.5 operations（核心：8 个 FC）

每个 op 的核心子段：

```json
"<OpName>": {
  "requestTemplate":  [...],   // 请求帧字节序列
  "inputs":           {...},   // 操作级输入段 (静态/自增)
  "outputs":          {...},   // 操作级输出段 (derivedLength 派生长度)
  "responseParser":   {...}    // 响应解析规则
}
```

---

## 3. 各 FC 配置↔字节对照

### 3.1 FC01 / 02 / 03 / 04（读多路，共用模板）

| op 名 | FC | 数据区 | 数量单位 |
|:--|:-:|------|:--:|
| `ReadCoils` | 0x01 | 0xxxx | bit |
| `ReadDiscreteInputs` | 0x02 | 1xxxx | bit |
| `ReadHoldingRegisters` | 0x03 | 4xxxx | 16-bit word |
| `ReadInputRegisters` | 0x04 | 3xxxx | 16-bit word |

**共同请求模板**：

```json
"requestTemplate": [
  "{TransactionID:X4}",   // 偏移 0..1: 自动递增事务 ID
  "{ProtocolID:X4}",            // 偏移 2..3: 固定 0x0000
  "00 06",                      // 偏移 4..5: 固定 Length=6
  "{UnitID:X2}",                // 偏移 6:   从站地址
  "<FC>",                       // 偏移 7:   功能码
  "{StartAddress:X4}",          // 偏移 8..9: 起始地址
  "{RegisterCount:X4}"          // 偏移 10..11: 读取数量
]
```

**对应 Modbus 字节**：

| 字节 | 模板元素 | 实际值示例 |
|:-:|---------|----------|
| 0..1 | `{TransactionID:X4}` | `00 01`（自增）|
| 2..3 | `{ProtocolID:X4}` | `00 00` |
| 4..5 | `"00 06"` | `00 06`（PDU 长度=6）|
| 6   | `{UnitID:X2}` | `01` |
| 7   | `"01"` / `"02"` / `"03"` / `"04"` | FC |
| 8..9 | `{StartAddress:X4}` | `00 00`（地址 0）|
| 10..11 | `{RegisterCount:X4}` | `00 0A`（读 10 个）|

**响应解析**：

```json
"responseParser": {
  "validCondition": "resp[7] == 0x<FC>",
  "dataStartIndex": 9,
  "dataLengthExpr": "resp[8]"
}
```

- `validCondition` 校验 Func 字段：第 7 字节（MBAP 后偏移 1）= FC
- `dataStartIndex=9` 数据区起点（MBAP 后偏移 3 = ByteCount 之后）
- `dataLengthExpr="resp[8]"` 数据长度 = 响应里的 ByteCount 字段

**响应字节示例**（读 10 个保持寄存器 FC03）：

```
00 01  ← TransactionID
00 00  ← ProtocolID
00 19  ← Length=25 (后续字节数)
01    ← UnitID
03    ← Func
14    ← ByteCount=20 (10 寄存器 * 2 字节)
XX XX ... (20 字节数据)
```

### 3.2 FC05 / FC06（写单值）

#### FC05 WriteSingleCoil

```json
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "00 06",
  "{UnitID:X2}",
  "05",                         // FC05
  "{StartAddress:X4}",
  "{WriteValue:X4}"             // 0xFF00=ON, 0x0000=OFF
]
```

**注意**：Modbus FC05 写线圈**没有 0/1 概念**，值必须是 `0xFF00`（ON）或 `0x0000`（OFF）。
- `tags.json` 里设 `WriteValue: 1` → 渲染为 `0001` ❌
- 应用层负责转换：**1 → 0xFF00，0 → 0x0000**
- 或者在 `operation` 级别做 `valueConverter`

**响应**：

```
[TransID--][ProtID][Length=6][UnitID][05][StartAddr][EchoValue]
```

- `validCondition`: `resp[7] == 0x05`
- `dataStartIndex: 12`，`dataLengthExpr: null`（写单值无数据区）

#### FC06 WriteSingleRegister

```json
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "00 06",
  "{UnitID:X2}",
  "06",                         // FC06
  "{StartAddress:X4}",
  "{WriteValue:X4}"             // 16-bit 寄存器值
]
```

**响应**：同 FC05，Func=0x06。

### 3.3 FC0F / FC10（写多值，**PDU 长度需计算**）

**关键问题**：PDU 长度 = `7 + ByteCount`，且 `RegisterCount`、`ByteCount` 都随写入数据变化，**不能**写死。

**MyProt 解决方案**：在写多操作的 **`outputs`** 段声明 `source=auto strategy=derivedLength` 变量，引擎按「变长载荷字节数」**自动计算** `RegisterCount`、`ByteCount`、`PDULength`（以纯 `expr` 表达，用 `{WriteValue:len}` 引用载荷字节数）。应用层**只需提供 `{WriteValue:raw}` 载荷字节**，数量与长度均由载荷派生，无需手工设置。

#### FC0F WriteMultipleCoils

```jsonc
// 写操作 inputs 段（载荷名仅 UI 展示；载荷由模板 {WriteValue:raw} 自动识别）
"inputs": {
  "WriteValue": { "source": "static", "label": "写入载荷" }
},
// 写操作 outputs 段（derivedLength 自动计算数量与长度）
"outputs": {
  "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} / 2", "label": "写入数量", "unit": "个" },
  "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}",     "label": "字节数", "unit": "字节" },
  "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{Frame:fixed} - 6 + {WriteValue:len}", "label": "PDU 长度" }
},
```

**派生公式**：以载荷字节数 `{WriteValue:len}` 为基准

- `RegisterCount` = `{WriteValue:len} * 8`（字节 → 线圈数，1 字节含 8 线圈）
- `ByteCount` = `{WriteValue:len}`（数据字节数）
- `PDULength` = `{Frame:fixed} - 6 + {WriteValue:len}`（MBAP Length = UnitID+Func+StartAddr+Qty+ByteCount+Data）

例：写 8/16 线圈（block）→ `{WriteValue:len}=1/2` → `RegisterCount=8/16`、`ByteCount=1/2`、`PDULength=8/9`。注意线圈按**位**打包，非整字节数量（如 3 个线圈仍占 1 字节）时 `{WriteValue:len}*8` 只会估到整字节上限（8）——需要精确数量请写请求显式带 `RegisterCount`（显式优先，覆盖派生值）。

**应用层责任**：仅提供 `{WriteValue:raw}` = N 字节打包后的 hex 字符串（`{WriteValue:raw}` 占位符直接拼入）；`RegisterCount`/`ByteCount`/`PDULength` **均自动计算**。

**示例**：写 16 个线圈，前 8 个为 1、后 8 个为 0

```json
{
  "WriteValue": "FF 00"        // raw: 2 字节（16 线圈打包）
}
```

实际请求帧（`RegisterCount`/`ByteCount`/`PDULength` 由载荷自动派生）：

```
00 01   ← TransactionID
00 00   ← ProtocolID
00 09   ← Length = {WriteValue:len}+7 = 9
01      ← UnitID
0F      ← Func
00 00   ← StartAddress
00 10   ← RegisterCount = {WriteValue:len}*8 = 16
02      ← ByteCount = {WriteValue:len} = 2
FF 00   ← WriteValue:raw (2 字节直接拼接)
```

#### FC10 WriteMultipleRegisters

```jsonc
// 写操作 inputs 段（载荷名仅 UI 展示；载荷由模板 {WriteValue:raw} 自动识别）
"inputs": {
  "WriteValue": { "source": "static", "label": "写入载荷" }
},
// 写操作 outputs 段（derivedLength 自动计算数量与长度）
"outputs": {
  "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} / 2", "label": "写入数量", "unit": "个" },
  "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}",     "label": "字节数", "unit": "字节" },
  "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7", "label": "PDU 长度" }
},
"requestTemplate": [
  "{TransactionID:X4}",
  "{ProtocolID:X4}",
  "{PDULength:X4}",            // ← derivedLength 派生注入（{Frame:fixed}-6+{WriteValue:len}）
  "{UnitID:X2}",
  "10",                        // FC16 = 0x10
  "{StartAddress:X4}",
  "{RegisterCount:X4}",        // ← derivedLength 派生注入（{WriteValue:len}/2）
  "{ByteCount:X2}",            // ← derivedLength 派生注入（{WriteValue:len}）
  "{WriteValue:raw}"              // ← 调用方提供的载荷字节（寄存器值，大端）
]
```

**派生公式**：以载荷字节数 `{WriteValue:len}` 为基准

- `RegisterCount` = `{WriteValue:len} / 2`（字节 → 寄存器数，每寄存器 2 字节）
- `ByteCount` = `{WriteValue:len}`（数据字节数）
- `PDULength` = `{WriteValue:len} + 7`

例：写 2 个寄存器 → `{WriteValue:len}=4` → `RegisterCount=2`、`ByteCount=4`、`PDULength=11`。

**应用层责任**：仅提供 `{WriteValue:raw}` = `RegisterCount*2` 字节的大端 hex（`{WriteValue:raw}` 占位符直接拼入）；`RegisterCount`/`ByteCount`/`PDULength` **均自动计算**。

**示例**：写 2 个寄存器，值为 0x0001 和 0x0002

```json
{
  "WriteValue": "00 01 00 02"   // raw: 4 字节大端（{WriteValue:len}=4）
}
```

**响应**：

```
[TransID--][ProtID][Length=6][UnitID][10][StartAddr][RegisterCount]
```

- `validCondition`: `resp[7] == 0x10`
- `dataStartIndex: 12`（响应里没有 ByteCount，只有 StartAddr + RegisterCount）

---

## 4. 异常响应处理（v1 局限）

### 4.1 行为

`validCondition` 不通过 → 整个响应判无效 → `quality=Bad` + `lastError.code=InvalidResponse`。

**不会**自动区分：
- 0x01 Illegal Function
- 0x02 Illegal Data Address
- 0x03 Illegal Data Value
- 0x04 Slave Device Failure

### 4.2 故障排查步骤

1. **看 [Data] 日志**：
   ```
   [WARN][Data] PLC-001.PLC-001.Pressure quality=Bad
   ```
2. **看 Wireshark**（过滤 `tcp.port == 502`）：
   - 找到 MyProt 发出的请求 → 对照响应
   - 响应 Func 字段 = `0x83`（= 0x03 + 0x80）→ 异常
   - 响应 byte[8] = `0x02` → Illegal Data Address（地址越界）
3. **修复**：
   - 0x01：协议层不支持该 FC → 检查 op 名
   - 0x02：`StartAddress` 越界 → 改小
   - 0x03：`WriteValue` 不在协议允许范围 → 调整

### 4.3 想细化异常识别

如果想在 `lastError` 里区分 0x01/0x02/0x03/0x04，可在 `validCondition` 加分支：

```json
"validCondition": "(resp[7] == 0x03) || (resp[7] == 0x83)"
```

**但目前不暴露 ExceptionCode**——为改进项，见 [ROADMAP.md](../ROADMAP.md)。

---

## 5. 实战示例

### 5.1 第三方仿真器（Modbus Slave / mbpoll）

#### 使用 diagslave（开源，免费）：

```bash
# 启动 diagslave 监听 502, 支持 FC01-06
diagslave -m tcp -p 502

# 写 1 个保持寄存器 (HR0 = 0x1234)
# 用 diagslave 的命令行:
# 实际上 diagslave 需配合 mbpoll 读写
```

#### 使用 mbpoll 测试：

```bash
# 测试 FC03 读 HR 4 (Modbus 地址 5, 1 个)
mbpoll -m tcp -p 502 -a 1 -t 4 -r 5 -c 1 -1 127.0.0.1

# 参数:
# -m tcp: Modbus TCP
# -p 502: 端口
# -a 1: UnitID=1
# -t 4: Holding Register
# -r 5: 寄存器地址 5 (mbpoll 从 1 算; MyProt 从 0 算 → MyProt StartAddress=4)
# -c 1: 数量 1
# -1: 单次读
```

### 5.2 MyProt 自带仿真器（[SimulationServer](../modules/10_Simulation.md)）

启动时无需额外配——**协议 JSON 自描述支持**：

- `modbus-tcp.json` 里声明的 8 个 FC，仿真器**全部支持**
- 启动时看日志：
  ```
  [INFO][Simulation] 协议 modbus-tcp 解析: 8 个操作
  [INFO][Simulation] 监听 0.0.0.0:11520
  ```
- 设备的 `connection.host` 设为 `127.0.0.1`，`connection.port` 设为 `11520`
- **省去第三方仿真器**——一站式测全 FC 集

### 5.3 完整 tags.json 配置（**多 FC 混合**）

```json
{
  "schemaVersion": 1,
  "devices": [{
    "id": "PLC-001",
    "protocol": "modbus-tcp",
    "connection": { "host": "127.0.0.1", "port": 502, "timeoutMs": 3000 }
  }],
  "tags": [
    {
      "name": "PLC-001.Temperature",
      "deviceId": "PLC-001",
      "operation": "ReadHoldingRegisters",
      "variables": { "ProtocolID": 0, "StartAddress": 2, "RegisterCount": 1 },
      "scanRateMs": 1000,
      "finalType": "Int16",
      "reportMode": "OnChange"
    },
    {
      "name": "PLC-001.Pressure",
      "deviceId": "PLC-001",
      "operation": "ReadInputRegisters",
      "variables": { "ProtocolID": 0, "StartAddress": 1, "RegisterCount": 1 },
      "scanRateMs": 1000,
      "finalType": "UInt16",
      "reportMode": "Always"
    },
    {
      "name": "PLC-001.Status",
      "deviceId": "PLC-001",
      "operation": "ReadCoils",
      "variables": { "ProtocolID": 0, "StartAddress": 0, "RegisterCount": 1 },
      "scanRateMs": 5000,
      "finalType": "Bool",
      "reportMode": "OnChange"
    },
    {
      "name": "PLC-001.Setpoint",
      "deviceId": "PLC-001",
      "operation": "WriteSingleRegister",
      "variables": { "ProtocolID": 0, "StartAddress": 10 },
      "writable": true
    }
  ]
}
```

---

## 6. 常见排错

| 现象 | 原因 | 解决 |
|------|------|------|
| `[Data] quality=Bad` + `[Channel] asio=10061` | 502 端口无人 | 启仿真器 / 改 host |
| `[Data] quality=Bad` 持续 30s+ | 仿真器异常响应 (Func=0x83) | 看 Wireshark + 改 StartAddress |
| 启动时 `[ERROR][Config] ... 配置根语义校验失败` | 模板变量未在 tag.variables / protocol.inputs(static/auto) 声明 | 加 protocol.inputs 静态/自增声明或 tag.variables |
| `[Channel] TCP 连接超时` | timeoutMs=0 | 改 3000+ |
| 写多寄存器无响应 | 模板 `{PDULength:X4}` 未在 outputs 声明 derivedLength | 在协议/op `outputs` 段声明 `PDULength`（expr 用 `{WriteValue:len}` 引用载荷字节数） |
| 实际数据错位 | `dataStartIndex` 错 | 重新对帧结构表 |

---

## 7. 进阶：自定义 FC

如果将来要加非标准 FC（如 FC43 / FC90）：

1. **在 `operations` 段加新条目**：
   ```json
   "ReadDeviceIdentification": {
     "requestTemplate": [
       "{TransactionID:X4}",
       "{ProtocolID:X4}",
       "00 06",
       "{UnitID:X2}",
       "2B",                    // FC43 (0x2B)
       "0E",                    // MEI Type 14
       "01",                    // Read Device ID
       "00"                     // Object ID 0
     ],
     "responseParser": { ... }
   }
   ```

2. **如果 PDU 长度依参数计算**（如 FC43 变长响应）：
   - **不要**走模板字面量 `"00 06"`
   - 改在协议/操作 `outputs` 段用 `{PDULength:X2}` 占位符 + 声明 `source=auto strategy=derivedLength`（`expr` 用 `{载荷名:len}` 引用载荷字节数，如 `{WriteValue:len} + 7`），引擎自动注入。
   - PDULength 一律走 `derivedLength` 声明求值。

3. **同步配置示例**：[configs/tags.json](../../configs/tags.json) 加 tag 项。

---

## 8. 关联索引

- 配置 Schema: [docs/Config_Schema.md](../Config_Schema.md)
- 8 模块文档: [docs/README.md](../README.md)
- 协议层: [docs/modules/02_Engine.md](../modules/02_Engine.md)
- 传输层: [docs/modules/03_Transport.md](../modules/03_Transport.md)
- PDU 长度注册表: [docs/modules/05_Gateway.md §5.3](../modules/05_Gateway.md#53-写路径)
- 仿真器: [docs/modules/10_Simulation.md](../modules/10_Simulation.md)
- 扩展候选: [docs/ROADMAP.md](../ROADMAP.md)

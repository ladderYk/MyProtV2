# 仙工智能（SEER）AGV 控制器协议教学（0x5A 0x01 + JSON 正文）

> 本文面向**已了解仙工控制器 TCP 接口、想把 MyProt 配置映射到实际字节**的读者。
>
> **前置知识**：仙工 API 的 TCP request/response 形态（**机器人作服务端**）；JSON 文本正文。
>
> **关联文件**：
> - 协议配置：[configs/protocols/seer.json](../../configs/protocols/seer.json)
> - 标签侧：[configs/tags.json](../../configs/tags.json) 的 `AGV1` 设备与四个标签
> - 配置 Schema：[docs/Config_Schema.md](../Config_Schema.md)
> - 姊妹文档：[modbus-tcp.md](./modbus-tcp.md)、[s7-1200.md](./s7-1200.md)（同构体例）
>
> **证据分级（与 s7-1200.md 同约定）**：
>
> | 标记 | 含义 |
> |:--:|------|
> | ✅ | **仓库内已验证**：`src/Tests/E2EMain.cpp` 的 `Test 21: SEER (Seer AGV) Byte-Level Wiring` 有对应断言（31 条，见[附录 §8](#8-附录证据清单test-21)）|
> | 🧮 | **推导**：由配置模板逐字节数出、或由 ✅ 线性推出（算术可复核）|
> | ⚠️ | **待官方手册核对**：本仓无真机、无官方手册，这些点以 v1 归档适配为基线，需你用手册确认 |

---

## 0. 30 秒速览

```jsonc
// seer.json 顶层 6 个字段  (移植自 archive/MyProtCpp/protocols/SEER.json)
{
  "transport": "Tcp（端口由设备侧 11500 指定）",
  "framing":   "LengthField @偏移 4 / 长 4 / 不含头 / headerLength 8",
  "dataByteOrder": "BigEndian（JSON 正文为 ASCII 字节，字节序仅影响数值型载荷）",
  "inputs":    "Seq（自增，2 字节）",
  "operations": "GetRobotStatus / GetPosition / GetBattery / SendJson(整包写)"
}
```

| 层级 | 字节 |
|------|------|
| 帧头（16B）| `5A 01`(2) + `Seq`(2) + `Length`(4) + `ApiType`(2) + 保留(6) |
| 正文 | **JSON 文本**（UTF-8/ASCII 字节原样）|

**与 Modbus / S7 最大的不同**：正文是**文本**。V2 的模板文法只接受 **hex 字面量 + 占位符**，所以正文要写成"JSON 文本的 hex"（`{"cmd":"status"}` → `7B 22 63 6D 64 22 3A 22 73 74 61 74 75 73 22 7D`）。这是本适配唯一"不优雅"的地方，已在 [ROADMAP](../ROADMAP.md) 登记缺口。

---

## 1. 帧结构

### 1.1 布局

```
偏移:  0  1 │ 2  3 │ 4  5  6  7 │ 8  9 │ 10 .. 15 │ 16 ..
内容: 5A 01│ Seq  │  Length     │ApiType│  保留    │ JSON 正文（N 字节）
      └魔数┘└─序号─┘└──长度(4B)──┘└─类型─┘└─6 字节─┘
```

| 偏移 | 字段 | 长度 | 本配置取值 | 说明 |
|:--:|------|:--:|------|------|
| 0..1 | 魔数 | 2 | `5A 01` | 请求固定头（响应同样以 `0x5A` 开头 ✅ 6-…见 21-8）|
| 2..3 | `Seq` | 2 | 自增（seed=1）| 请求序号，`{Seq:X4}` ✅ 21-4 |
| 4..7 | `Length` | 4 | 见 [§1.2](#12-长度域语义最关键的一节-) | 大端 32 位 |
| 8..9 | `ApiType` | 2 | 每个操作固定（如 `07 D0`=2000）| 接口编号 ✅ 21-4 |
| 10..15 | 保留 | 6 | 全 `00` | 本配置不校验其内容 ✅ 21-4 |
| 16.. | 正文 | N | JSON 文本 | 读请求为固定正文；写请求为运行时注入 ✅ 21-4/21-6 |

### 1.2 长度域语义（最关键的一节 ★）

V2 的 LengthField 解析器（`src/Transport/src/LengthFieldFrameParser.cpp`）与 Schema 定义：

```
lengthIncludesHeader = false  →  总帧长 = headerLength + 长度值 + lengthAdjustment
其中 headerLength = 「帧首至长度字段结束」的字节数 = lengthFieldOffset + lengthFieldLength = 4 + 4 = 8
```

⇒ **长度值 = 总帧长 − 8**（即"长度域之后的全部字节"：`ApiType(2) + 保留(6) + 正文(N)` = N + 8）

**本配置据此取值** ✅ 21-4：

| 操作 | 正文 | 总帧长 | 长度值 |
|------|:--:|:--:|:--:|
| `GetRobotStatus` | 16 B | 32 | 24 (`0x18`) |
| `GetPosition` | 13 B | 29 | 21 (`0x15`) |
| `GetBattery` | 17 B | 33 | 25 (`0x19`) |
| `SendJson`（写）| N | 16 + N | N + 8 |

**若与手册不符，只改一个数**（⚠️ 请对照官方手册的"长度字段"说明）：

| 手册里的长度字段含义 | 长度值应等于 | 本配置要改的地方 |
|------|------|------|
| 长度域之后的字节（本配置假设）| 总长 − 8 | 无需改（`lengthAdjustment: 0`）|
| **正文字节数** | N | 读三连的字面量各减 8（24→16、21→13、25→17），`SendJson` 的 expr 改 `{Body:len}` |
| **含 16 字节头**（总帧长）| 总长 | `lengthAdjustment: -8`，读三连字面量各 +8，`SendJson` 的 expr 改 `{Body:len} + 16` |

> v1 归档配置写的是 `headerLength: 16` —— 那是**旧引擎的语义**；V2 的 `headerLength` 定义是"帧首至长度域结束"（对照 `modbus-tcp.json` 的 6 = 4+2、`s7-1200.json` 的 4 = 2+2 即可确认）✓ 所以移植时必须改成 **8**，否则整帧会多算 8 字节 ✅（21-7 用 32B/31B 两条断言把语义钉住了）。

---

## 2. 配置逐字段对照

### 2.1 transport 与设备侧

```jsonc
// seer.json —— 协议只声明类型（V2 已弃用协议级 defaultPort）
"transport": { "type": "Tcp" }
```

```jsonc
// tags.json —— 设备侧给出 host/port/超时（示例：AGV1）
{
  "id": "AGV1",
  "protocol": "seer",
  "connection": { "host": "192.168.1.100", "port": 11500, "timeoutMs": 3000 },
  "requestTimeoutMs": 3000
}
```

⚠️ `192.168.1.100:11500` 取自 v1 归档配置，**请改成你控制器的实际 IP**；端口以官方手册为准。

### 2.2 framing

```jsonc
"framing": {
  "type": "LengthField",
  "lengthFieldOffset": 4,        // 长度域起始偏移
  "lengthFieldLength": 4,        // 4 字节（大端）
  "lengthIncludesHeader": false, // 长度值不含帧头
  "byteOrder": "BigEndian",
  "headerLength": 8,             // ★ = 帧首至长度域结束（4+4），不是 16
  "lengthAdjustment": 0,
  "maxFrameSize": 4096
}
```

### 2.3 inputs

| 变量 | 配置 | 说明 |
|------|------|------|
| `Seq` | `source: auto` + `autoIncrement`（seed=1）| 每次请求自增；若手册要求"一次会话内固定序号"，改为 `static` 并在标签 `variables` 里给值 |

> `ApiType` **没有**做成变量：三个读操作的接口编号是固定的，直接写进各自模板的 hex 字面量（`07 D0` / `07 D1` / `07 D3`）✅ 21-4 —— 比 v1 的"每个标签写一遍 ApiType"少一个出错面。

### 2.4 operations

| 操作 | kind | 正文 | 用途 |
|------|:--:|------|------|
| `GetRobotStatus` | read | `{"cmd":"status"}` | 机器人状态 |
| `GetPosition` | read | `{"cmd":"pos"}` | 位姿 |
| `GetBattery` | read | `{"cmd":"battery"}` | 电量 |
| `SendJson` | write | `{Body:raw}`（调用方注入）| **整包下发**（含 Relocate 等带参命令）|

---

## 3. 逐字节对照

### 3.1 读三连 ✅ 21-4

以 `GetRobotStatus` 为例（`:16` 表示从偏移 16 起）：

```
5A 01                         魔数
00 01                         Seq（seed=1 首次）
00 00 00 18                   长度值 24 = 32 − 8
07 D0                         ApiType 2000
00 00 00 00 00 00             保留
7B 22 63 6D 64 22 3A 22 ...   正文 hex：{"cmd":"status"}
```

三条读请求的全部差异只有 3 处（长度值 / ApiType / 正文）✅：

| 操作 | 长度值 | ApiType | 正文 | 正文 hex |
|------|:--:|:--:|:--:|------|
| `GetRobotStatus` | `00 00 00 18` | `07 D0` | `{"cmd":"status"}` | `7B22636D64223A22737461747573227D` |
| `GetPosition` | `00 00 00 15` | `07 D1` | `{"cmd":"pos"}` | `7B22636D64223A22706F73227D` |
| `GetBattery` | `00 00 00 19` | `07 D3` | `{"cmd":"battery"}` | `7B22636D64223A2262617474657279227D` |

### 3.2 JSON 正文怎么变成 hex（自己算的方法）

逐字符取 ASCII 码即可，例如 `{"cmd":"pos"}`：

```
{  22 "   c   m   d   "   :   "   p   o   s   "   }
7B 22 63 6D 64 22 3A 22 70 6F 73 22 7D      ← 13 字节
```

> 想让 MyProt 直接吃文本正文（写 `$"{\"cmd\":\"pos\"}"` 这种），需要给模板文法加**文本行原语**——已登记在 [ROADMAP](../ROADMAP.md)（P2）。当前 A1 版本用 hex 编码。

### 3.3 整包写 `SendJson` ✅ 21-6

模板固定段 16 字节 + `{Body:raw}`：

```
5A 01 / {Seq:X4} / {Length:X8} / 27 10 / 00×6 / {Body:raw}
                              └ ApiType：占位值 0x2710 ⚠️ 待手册填写真实编号
```

- 长度值 = `{Body:len} + 8`（`derivedLength` ✅ 21-6：载荷 7 字节 → 长度值 15）
- 帧长 = 16 + 载荷 ✅ 21-6（载荷 7 → 23 字节）
- 载荷原样落在偏移 16 起 ✅ 21-6

**调用示例**（把 JSON 文本先编码成 hex）：

```bash
# {"x":1} = 7B 22 78 22 3A 31 7D
curl -s -X POST http://127.0.0.1:8080/api/data/write \
     -H 'Content-Type: application/json' \
     -d '{"tag":"AGV1.SendJson","bytes":"7B 22 78 22 3A 31 7D"}'

# Relocate 示例：{"x":1,"y":2,"angle":90}
#   = 7B 22 78 22 3A 31 2C 22 79 22 3A 32 2C 22 61 6E 67 6C 65 22 3A 39 30 7D
#   （记得把 ApiType 换成手册里"重定位"的接口编号）
```

> ⚠️ `SendJson` 的 `ApiType` 目前是**占位值** `0x2710`（v1 未给出 Relocate 编号）。用之前请按手册改成真实编号，并在本文件与本配置里同步。

---

## 4. 响应与判定

```jsonc
"responseParser": {
  "validCondition": "resp[0] == 0x5A",   // 魔数判定
  "dataStartIndex": 16,                  // 正文从 16 字节头之后开始
  "dataLengthExpr": ""                   // ★ 留空 = 帧长 − dataStartIndex（取到帧尾）
}
```

- **帧尾语义**：`dataLengthExpr` 留空时数据区长度 = 帧长 − 16（`Config.hpp` 对 `dataLengthExpr` 的定义）✅ 21-8（断言载荷 hex 与"响应偏移 16 起"逐字节相同）
- **判定失败的表现（本仓实测）**：`validCondition` 不通过 → **ResponseParser 直接返回错误**（不是"质量非 Good 的 TagValue"）✅ 21-8；在完整管线里由 `TagReader` 映射为 `quality`（见 [ADR-0006](../adr/0006-quality-semantics.md)）。
- **取值形态**：标签 `finalType: ByteArray` → 值以 **hex 字符串**呈现（如 `7B22636D64223A...`）。V2 没有"文本/JSON 解码"能力，所以 `{"battery":82}` 不会自动变成数字 `82` —— 需要 JSON 解析的话，属缺口（见 [§6](#6-局限与缺口登记)）。

标签侧示例：

| 标签 | 操作 | scanRateMs | finalType |
|------|------|:--:|------|
| `AGV1.RobotStatus` | `GetRobotStatus` | 1000 | `ByteArray` |
| `AGV1.RobotPosition` | `GetPosition` | 500 | `ByteArray` |
| `AGV1.BatteryLevel` | `GetBattery` | 5000 | `ByteArray` |
| `AGV1.SendJson` | `SendJson`（`direction: write`）| —（只写，不轮询）| `ByteArray` |

> 四个标签都 `coalesce: false`（单地址/整包语义，不参与地址合并）。

---

## 5. v1 归档 → V2 的映射（移植对照）

| v1 `SEER.json` 特性 | V2 等价物 | 判定 |
|------|------|:--:|
| `transport.defaultPort: 11500` | 设备侧 `connection.port` | ✅ 已等价移植 |
| `framing` 4/4/不含头 | 同名字段 | ✅ |
| `framing.headerLength: 16` | **改为 8**（V2 定义 = 帧首至长度域结束）| ✅ 已修正（21-3/21-7）|
| `{Length:calc:X8}` | `derivedLength`；读=常量字面量，写=`{Body:len} + 8` | ✅（21-4/21-6）|
| `dataLengthExpr: "*"` | **留空**（= 帧长 − `dataStartIndex`）| ✅（21-8）|
| `validCondition: resp[0] == 0x5A` | 同语法 | ✅ |
| `valueType: ByteArray`（responseParser 内）| 移到标签 `finalType: ByteArray` | ✅ |
| `Seq` / `ApiType` 由标签传 | `Seq` → auto 自增；`ApiType` → 模板字面量 | ✅ |
| `$"..."` 文本正文 + `$(X)` 替换 | ❌ **V2 无文本原语** → 正文改 hex；变量部分由调用方编码后走 `{Body:raw}` | ⚠️ 缺口（可绕过）|

---

## 6. 局限与缺口登记

| 项 | 现状 | 去向 |
|------|------|------|
| 模板文本行原语（`$"..."`、`$(Var)` 替换）| 不支持；正文需 hex 编码 | [ROADMAP](../ROADMAP.md)（P2，待评审）|
| 响应 JSON → 数值解码 | 不支持；值为 hex 字符串 | 同上（可与原语一并设计）|
| 内置仿真器 | **不适用于 SEER**：仿真器只取协议列表第 0 个协议 + 寄存器数据模型（见 [s7-1200.md §6.2](./s7-1200.md#62-被控端三种选择)）| 联调用真机或自建 stub |
| 错误码解析 | 只判 `resp[0]`，不解析业务错误字段 | 需要时按手册补 `validCondition` |

---

## 7. 排错

| 现象 | 优先检查 | 依据 |
|------|------|------|
| 所有标签 `Bad`、日志无响应 | `connection.host/port`（11500 是否为该控制器的 API 端口）| ⚠️ §2.1 |
| 收不到响应 / 响应被截断 | `headerLength` 是否为 **8**、长度值是否 = 总长 − 8 | ✅ §1.2 |
| 收到响应但判定失败 | `validCondition`（`resp[0] == 0x5A`）与手册的响应头/魔数是否一致 | ⚠️ §4 |
| 读到的"值"看不懂 | 这是**正文的 hex**（`ByteArray`），不是 JSON 解析结果 | ✅ §4 |
| 下发命令无效果 | ①`SendJson` 的 `ApiType` 仍是占位 `0x2710` ②正文 hex 是否漏/多字节（长度值会随之变化）| ⚠️ §3.3 |
| 配置保存被拒 | 看弹窗编号清单（如"帧长一致性失败"会直接指出操作名与长度槽位差多少）| ✅ §1.2 |

---

## 8. 附录：证据清单（Test 21）

`src/Tests/E2EMain.cpp` → `Test 21: SEER (Seer AGV) Byte-Level Wiring`（**全部基于随仓库发布的 `configs/protocols/seer.json`**，配置被改即回归报警）：

| # | 断言 |
|:--:|------|
| 21-1 | `seer.json` 通过深度校验（与保存期同一套，含 ADR-0012 试算）|
| 21-2 | `seer.json` 解析为 POCO |
| 21-3 | framing = headerLength 8 / offset 4 / len 4 / 不含头 |
| 21-4 ×3 操作 ×5 项 | `Build` 成功 / 帧长（32·29·33）/ 魔数 `5A 01` + Seq 自增 / 长度值（24·21·25）/ ApiType（`07D0`·`07D1`·`07D3`）/ 正文 hex 逐字节一致 |
| 21-5 / 21-6 | `SendJson` 操作存在 / `BuildBytes` 成功 / 帧长 23 / 长度值 15 / 载荷原样落在偏移 16 |
| 21-7 | 32B 响应切成 1 整帧（8 + 24）；31B 判为不完整 |
| 21-8 | 合法响应 → `quality = Good`；载荷 = 帧尾正文（`dataStartIndex` 16 + 空 `dataLengthExpr`）；首字节非 `0x5A` → 不被判定为 Good（实测为 ResponseParser 返回错误）|

**本文未验证的部分（诚实清单）**：

- 与真实控制器的连通、握手/时序要求（本仓无 AGV 真机）；
- 手册层面的字段含义：魔数 `5A 01`、长度域语义、`ApiType` 编号表（含 `SendJson` 的占位值）、响应头结构与业务错误字段、`Seq` 的回显/递增要求 ⚠️；
- 响应正文的 JSON 结构（各接口返回哪些字段）——需要手册或抓包；
- 除 status/pos/battery/整包写以外的接口（如任务下发、IO、导航）。

> 你手上已有官方手册：把上述 ⚠️ 项对照一遍，我可以把本文与本配置里的"待核对"逐条改成 ✅，并把 `SendJson` 的 `ApiType` 换成真实编号。

---

## 9. 关联索引

- 协议配置：[configs/protocols/seer.json](../../configs/protocols/seer.json)
- 标签侧：[configs/tags.json](../../configs/tags.json)（设备 `AGV1`）
- 归档基线：[archive/MyProtCpp/protocols/SEER.json](../../archive/MyProtCpp/protocols/SEER.json)（v1 适配）
- 姊妹文档：[modbus-tcp.md](./modbus-tcp.md)、[s7-1200.md](./s7-1200.md)、[protocols/README.md](./README.md)
- 契约：[Config_Schema.md](../Config_Schema.md)（模板文法、`derivedLength`、`dataLengthExpr`、`coalesce`）
- 决策：[ADR-0012 派生长度原语与保存期试算校验](../adr/0012-derivedlength-template-primitives-and-trial-render.md)、[ADR-0006 QualityCode 语义](../adr/0006-quality-semantics.md)
- 未实装扩展：[ROADMAP.md](../ROADMAP.md)

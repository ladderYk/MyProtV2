# 欧姆龙 FINS/TCP 协议教学（`FINS` 魔数 + 大端长度域）

> 本文面向**已了解欧姆龙 FINS/TCP 命令帧、想把 MyProt 配置映射到实际字节**的读者。
>
> **前置知识**：FINS/TCP 由 8 字节 TCP 前缀（`FINS` 魔数 + 长度 + command + error）与被包住的 FINS 帧（10 字节头 + 命令 + 参数）组成；设备作**服务端**。
>
> **关联文件**：
> - 协议配置：[configs/protocols/omron-fins-tcp.json](../../configs/protocols/omron-fins-tcp.json)
> - 配置 Schema：[docs/Config_Schema.md](../Config_Schema.md)
> - 姊妹文档：[modbus-tcp.md](./modbus-tcp.md)、[mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md)、[twincat-ads.md](./twincat-ads.md)
>
> **证据分级**：
>
> | 标记 | 含义 |
> |:--:|------|
> | ✅ | **仓库内已验证**：`src/Tests/E2EMain.cpp` 的 `Test 22` 有对应断言（22-1 … 22-6b）|
> | 🧮 | **推导**：由模板逐字节数出，或由 ✅ 线性推出（算术可复核）|
> | ⚠️ | **待手册/真机核对**：本仓无真机，以下点按公开手册整理，需你确认 |

---

## 0. 30 秒速览

```jsonc
// omron-fins-tcp.json 的骨架
{
  "transport": "Tcp, defaultPort 9600",
  "framing": {
    "type": "LengthField",          // 长度域: 偏移 0 / 长 4 / 大端
    "lengthIncludesHeader": false,  // 长度值不含 TCP 前缀
    "headerLength": 8               // 长度值 = 总帧长 - 8
  },
  "dataByteOrder": "BigEndian",     // FINS 字数据为大端
  "inputs": "路由 9 项 (GCT/DNA/DA1/DA2/SNA/SA1/SA2) + 区代码 + SID 自增",
  "operations": "ReadWordArea (0101 内存区读)"
}
```

| 层级 | 字节 |
|------|------|
| TCP 前缀（8B）| `FINS`(4) + `Length`(4, BE) |
| TCP command/error（8B）| `00 00 00 02` + `00 00 00 00` |
| FINS 头（10B）| `ICF`(1) `RSV`(1) `GCT`(1) `DNA`(1) `DA1`(1) `DA2`(1) `SNA`(1) `SA1`(1) `SA2`(1) `SID`(1) |
| 命令 + 参数 | `MRC SRC`(2) + 区代码(2) + 起始字(2) + 字数(2) |

---

## 1. 帧结构

### 1.1 布局（读 WR 区，1 字）

```
偏移: 0  1  2  3 │ 4  5  6  7 │ 8  9 10 11 │ 12 13 14 15 │16│17│18│19│20│21│22│23│24│25│26 27│28 29│30 31│32 33│
内容:  F  I  N  S │ Length(BE) │  00 00 00 02│  00 00 00 00│80│00│GCT│DNA│DA1│DA2│SNA│SA1│SA2│SID│01 01│B3 00│Start│Count│
      └── 魔数 ──┘ └── 长度 ──┘ └── command ┘└── error ──┘└────── FINS header ──────┘└命令─┘└区代码┘└字地址┘└字数─┘
```

| 偏移 | 字段 | 长度 | 本配置取值 | 说明 |
|:--:|------|:--:|------|------|
| 0..3 | `FINS` 魔数 | 4 | ASCII `46 49 4E 53` | ✅ 22-2 |
| 4..7 | `Length` | 4 | `总帧长 - 8` | 大端；34B 帧 → `00 00 00 1A` ✅ 22-3 |
| 8..11 | TCP command | 4 | `00 00 00 02` | 2 = FINS 帧 ✅ 22-3b |
| 12..15 | TCP error | 4 | `00 00 00 00` | 请求恒 0 |
| 16 | `ICF` | 1 | `80` | 命令帧 ✅ 22-3c |
| 17 | `RSV` | 1 | `00` | 保留 ✅ |
| 18 | `GCT` | 1 | `02` | 网关数 |
| 19 | `DNA` | 1 | `00` | 目标网络号 |
| 20 | `DA1` | 1 | 设备节点号（缺省 20）| ✅ 22-6b（`devices[].variables` 覆盖）|
| 21 | `DA2` | 1 | `00` | 目标单元号（CPU=0）|
| 22 | `SNA` | 1 | `00` | 源网络号 |
| 23 | `SA1` | 1 | 源节点号（缺省 1）| ✅ 22-6b |
| 24 | `SA2` | 1 | `00` | 源单元号 |
| 25 | `SID` | 1 | 自增（seed=0）| 服务标识，0x00–0xFF 循环 |
| 26..27 | `MRC SRC` | 2 | `01 01` | 内存区读 ✅ 22-4 |
| 28..29 | 区代码 | 2 | `B3 00` | WR 区（`00` 为副码）✅ 22-5 |
| 30..31 | 起始字 | 2 | `StartByteAddress / 2` | 大端 ✅ 22-6（100 → `00 32`）|
| 32..33 | 字数 | 2 | `ByteCount / 2` | 大端 ✅（2 → `00 01`）|

### 1.2 长度域语义（最容易写错的一处 ★）

`LengthField` 的三个字段共同决定"长度值"：

| 配置 | 值 | 含义 |
|------|---|------|
| `lengthFieldOffset` | 0 | 长度域从帧首开始 |
| `lengthFieldLength` | 4 | 4 字节 |
| `lengthIncludesHeader` | `false` | 长度值**不含**前 `headerLength` 字节 |
| `headerLength` | 8 | 即 `FINS` 魔数 + 长度域本身 |
| `lengthAdjustment` | 0 | 无额外修正 |

因此 `{Len:X8}` 的表达式是 `{Frame:fixed} - 8`（✅ 22-3：34 − 8 = 26 = `0x1A`）。

> **不要**把长度写成"总帧长"：那会多算 8 字节，设备会把帧尾当下一帧的开头。

---

## 2. 配置逐字段对照

### 2.1 transport / 设备侧

```jsonc
"transport": { "type": "Tcp", "defaultPort": 9600 }
```

FINS/TCP 缺省端口 `9600`；实际端口以设备以太网单元设置为准。设备在 `configs/tags.json` 中接入：

```jsonc
{
  "id": "PLC-OMRON-1",
  "protocol": "omron-fins-tcp",
  "connection": { "host": "192.168.1.10", "port": 9600 },
  // 同型号设备只差节点号时不要复制协议文件:
  "variables": { "DA1": 21, "SA1": 2 }
}
```

### 2.2 inputs（协议常量与路由）

| 名称 | 缺省 | 作用 |
|------|:--:|------|
| `StartByteAddress` | 0 | **字节**地址（引擎统一契约）；WR 区按 `÷2` 换成字地址 |
| `ByteCount` | 2 | 数据字节跨度（读 2 字节 = 1 字）|
| `GCT` / `DNA` / `DA1` / `DA2` / `SNA` / `SA1` / `SA2` | 2/0/20/0/0/1/0 | FINS 路由字段，通常只改 `DA1`/`SA1` |
| `SID` | 自增 | 服务标识 |
| `MemKind` | `0xB3` | 内存区代码；DM 区改为 `0x82`（副码 `00`）|

> 路由字段按**设备**覆盖（`devices[].variables`），标签只写 `StartByteAddress` / `ByteCount` 即可。

### 2.3 outputs（派生进帧的字节）

| 名称 | 表达式 | 说明 |
|------|--------|------|
| `Len` | `{Frame:fixed} - 8` | TCP 长度域 |
| `StartWordHi/Lo` | `hi8(StartByteAddress / 2)` / `lo8(...)` | 起始字地址拆高低字节 |
| `WordCntHi/Lo` | `hi8(ByteCount / 2)` / `lo8(...)` | 字数拆高低字节 |

`hi8/lo8` 是表达式内建函数（替代 `x / 256 % 256` 手写算术），见 [Config_Schema §2.4](../Config_Schema.md#24-占位符模板)。

### 2.4 responseParser

```jsonc
"responseParser": {
  "validCondition": "resp[28] == 0 && resp[29] == 0",  // MRES/SRES = 00 00
  "dataStartIndex": 30,                                // 数据区起点
  "dataLengthExpr": ""                                 // 空 = 取到帧尾
}
```

响应帧的 FINS 头与请求同构：`26..27` 是命令回显，`28..29` 才是 **MRES/SRES 响应码**，数据从 `30` 开始（⚠️ 按公开手册整理，本仓无真机；早期配置写成 `26`/`28` 会把命令回显当响应码、并把响应码混进数据区）。

---

## 3. 逐字节示例

`StartByteAddress = 100`（字地址 50 = `0x0032`）、`ByteCount = 2`（1 字）、缺省路由 `DA1=20` / `SA1=1`：

```
46 49 4E 53  00 00 00 1A  00 00 00 02  00 00 00 00
80 00 02 00 14 00 00 01 00 00  01 01  B3 00  00 32  00 01
└─ FINS ───┘ └ 长度=26 ─┘ └ command ─┘ └ error ──┘
└ ICF/RSV ┘ GCT DNA DA1 DA2 SNA SA1 SA2 SID  命令   区   起始字 字数
```

34 字节，与 `22-1` / `22-6` 的断言一致；把 `DA1` 改成 21、`SA1` 改成 2 后，偏移 20 / 23 的字节随之变化（✅ 22-6b）。

---

## 4. 从零接入一台新 PLC

1. **协议**：直接使用 `configs/protocols/omron-fins-tcp.json`（同型号无需复制）。
2. **设备**：`configs/tags.json` → `devices[]` 增加一条，`protocol` 填 `omron-fins-tcp`，只差节点号时写 `variables: { "DA1": 21, "SA1": 2 }`。
3. **标签**：`tags[]` 增加一条，`operation` = `ReadWordArea`，`variables` 写 `StartByteAddress` / `ByteCount`：

```jsonc
{
  "name": "OMRON1.D100",
  "deviceId": "PLC-OMRON-1",
  "operation": "ReadWordArea",
  "direction": "read",
  "scanRateMs": 1000,
  "finalType": "UInt16",
  "variables": { "StartByteAddress": 100, "ByteCount": 2 }
}
```

4. **校验**：`MyProt.App.exe --validate configs`，再启动网关。

---

## 5. 常见坑

| 现象 | 原因 | 处理 |
|------|------|------|
| 设备不响应 | 长度域写成总帧长 / 字节序写成小端 | `{Frame:fixed} - 8`；`LengthField.byteOrder = BigEndian` |
| 读到错位数据 | `StartByteAddress` 直接写了字地址 | 填**字节**地址（字地址 × 2）；协议内部再 `÷2` |
| 响应被判非法 | 响应码位置写成 `26` | 响应码在 `28..29`，数据从 `30` 起 |
| 多台 PLC 只能通一台 | 复制了协议文件且只改一台 | 用 `devices[].variables` 覆盖 `DA1`/`SA1` |
| 读 DM 区失败 | 区代码仍是 WR（`0xB3`）| 改 `MemKind = 0x82` |

---

## 6. 附录：证据清单（Test 22）

| 断言 | 内容 |
|------|------|
| `22-0` | `configs/` 全目录加载通过（6 协议）|
| `22-1a` / `22-1` | FINS 请求构建成功，帧长 34 |
| `22-2` | 魔数 ASCII `FINS` |
| `22-3` / `22-3b` / `22-3c` | 长度 `00 00 00 1A`、command `00 00 00 02`、`ICF=80` |
| `22-4` / `22-5` | 命令 `01 01`、区代码 `B3 00` |
| `22-6` / `22-6b` | 起始字/字数 `00 32 / 00 01`；设备级变量覆盖进入帧 |

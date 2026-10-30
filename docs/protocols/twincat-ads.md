# 倍福 TwinCAT ADS/AMS 协议教学（AMS/TCP 外层 + 全字段小端）

> 本文面向**已了解 Beckhoff ADS/AMS 报文、想把 MyProt 配置映射到实际字节**的读者。
>
> **前置知识**：AMS/TCP 帧 = 6 字节外层（`reserved` + AMS 长度）+ AMS 头（目标/源 NetId+端口、命令、状态、数据长、错误码、调用 ID）+ ADS 载荷；TwinCAT 运行时作**服务端**（缺省端口 48898）。
>
> **关联文件**：
> - 协议配置：[configs/protocols/twincat-ads.json](../../configs/protocols/twincat-ads.json)
> - 配置 Schema：[docs/Config_Schema.md](../Config_Schema.md)
> - 姊妹文档：[omron-fins-tcp.md](./omron-fins-tcp.md)、[mitsubishi-mc-3e.md](./mitsubishi-mc-3e.md)
>
> **证据分级**：
>
> | 标记 | 含义 |
> |:--:|------|
> | ✅ | **仓库内已验证**：`src/Tests/E2EMain.cpp` 的 `Test 22` 有对应断言（22-14 … 22-20）|
> | 🧮 | **推导**：由模板逐字节数出 |
> | ⚠️ | **待手册/真机核对**：本仓无真机；NetId 与 IndexGroup 必须按你的工程确认 |

---

## 0. 30 秒速览

```jsonc
{
  "transport": "Tcp, defaultPort 48898",
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 2,          // reserved(2) 之后
    "lengthFieldLength": 4,
    "lengthIncludesHeader": false,
    "byteOrder": "LittleEndian",
    "headerLength": 6                // 长度值 = 总帧长 - 6
  },
  "dataByteOrder": "LittleEndian",
  "operations": "ReadByOffset (ADS Read, 命令 0x0002)"
}
```

| 层级 | 字节 |
|------|------|
| 外层（6B）| `00 00` + `AMS Length`(4, LE) |
| AMS 头（32B）| 目标(8) + 源(8) + 命令(2) + 状态(2) + 数据长(4) + 错误(4) + 调用 ID(4) |
| ADS 读请求（12B）| `IndexGroup`(4) + `IndexOffset`(4) + `Length`(4) |

---

## 1. 帧结构

### 1.1 布局（ADS Read，读 2 字节）

```
偏移: 0  1 │ 2  3  4  5 │ 6  7  8  9 │10 11│12 13│14 15 16 17│18 19│20 21│22 23│24 25│26 27 28 29│30 31 32 33│34 35 36 37│38 39 40 41│42 43 44 45│46 47 48 49│
内容: 00 00│ AMS Length │ 目标 NetId  │端口 │ 源 NetId   │端口 │命令 │状态 │ 数据长     │ 错误码     │ 调用 ID    │ IndexGroup│ IndexOffset│ 读取长度 │
      └外层┘└──── 4B ───┘└── 6B ─────┘└2B─┘└── 6B ─────┘└2B─┘└2B─┘└2B─┘└── 4B ─────┘└── 4B ────┘└── 4B ─────┘└── 4B ────┘└── 4B ─────┘└── 4B ────┘
```

| 偏移 | 字段 | 长度 | 本配置取值 | 说明 |
|:--:|------|:--:|------|------|
| 0..1 | reserved | 2 | `00 00` | 固定 |
| 2..5 | AMS 长度 `AdmLen` | 4 | `{Frame:fixed} - 6` | 小端；50B 帧 → `2C 00 00 00` ✅ 22-14b |
| 6..9 | 目标 NetId 低 32 位 | 4 | `NetIdLo` | 小端 ✅ 22-15 |
| 10..11 | 目标 NetId 高 16 位 | 2 | `NetIdHi` | 小端 ✅ |
| 12..13 | 目标端口 | 2 | `801` = `0x0321` → `21 03` | PLC 运行时 ✅ |
| 14..17 | 源 NetId 低 32 位 | 4 | `NetIdLo` | |
| 18..19 | 源 NetId 高 16 位 | 2 | `NetIdHi` | |
| 20..21 | 源端口 | 2 | `22880` = `0x5960` → `60 59` | 任意高位端口 |
| 22..23 | 命令 `CmdId` | 2 | `2` = Read ✅ 22-16 |
| 24..25 | 状态 `StateFlags` | 2 | `4` = Request ✅ 22-17 |
| 26..29 | 数据长 `DataLen` | 4 | `12`（3 × 4B）| ✅ 22-17b |
| 30..33 | 错误码 `ErrorInfo` | 4 | `0` | 请求恒 0 |
| 34..37 | 调用 ID `InvokeIDSeed` | 4 | 自增（seed=1）| 响应回显，用于关联 |
| 38..41 | `IndexGroup` | 4 | 61441（`0xF001`）| ⚠️ 按目标区/读取方式确认 |
| 42..45 | `IndexOffset` | 4 | `StartByteAddress` ✅ 22-18 |
| 46..49 | 读取长度 | 4 | `ByteCount` ✅ 22-19 |

### 1.2 长度域语义（★）

AMS/TCP 长度域在**偏移 2**、4 字节、小端，值 = **总帧长 − 6**：

```
50 - 6 = 44 (0x0000002C) → 线上 2C 00 00 00
```

模板里是 `{Frame:fixed} - 6`；写成整帧长会让对端把 6 字节外层当正文。

---

## 2. 配置逐字段对照

### 2.1 NetId 是怎么拼出来的（最容易错的一处）

AMS NetId 是 6 字节：`IP(4) + 2`，通常写作 `192.168.1.1.1.1`。本配置把它拆成两个整数：

| 变量 | 值 | 线上字节（LE）| 对应 NetId 字节 |
|------|:--:|------|------|
| `NetIdHi` | `0xC0A8` | `A8 C0` | `192.168` |
| `NetIdLo` | `0x00010101` | `01 01 01 00` | `.1.1.0` |

合起来 = `192.168.1.1.1.0`。若你的 PLC NetId 是 `192.168.1.1.1.1`，把 `NetIdLo` 改成 `0x01010101`；IP 不是 `192.168.x.x` 时两个值都要重算：

```
NetIdHi = 前两段 IP 按小端打包:  192.168 → 0xC0A8
NetIdLo = 后两段 + 主机段:       1.1.1.1 → 0x01010101
```

> **不要**用十进制"看着像 IP"的写法；这里是按小端整数编码的字节串。

### 2.2 inputs

| 名称 | 缺省 | 作用 |
|------|:--:|------|
| `StartByteAddress` / `ByteCount` | 0 / 2 | 引擎契约；分别落到 `IndexOffset` / 读取长度 |
| `NetIdLo` / `NetIdHi` | 见上 | 目标与源共用（同机通信）|
| `TargetPort` / `SourcePort` | 801 / 22880 | 目标运行时端口 / 本地端口 |
| `CmdId` / `StateFlags` | 2 / 4 | ADS Read + Request |
| `DataLen` | 12 | 读请求载荷 = 3 × 4B |
| `ErrorInfo` | 0 | 请求恒 0 |
| `InvokeIDSeed` | 自增 | 调用 ID |
| `IndexGroup` | 61441 | ⚠️ 见 §4 |

### 2.3 outputs

| 名称 | 表达式 | 说明 |
|------|--------|------|
| `AdmLen` | `{Frame:fixed} - 6` | AMS 长度域 |

### 2.4 responseParser

```jsonc
{
  "validCondition": "resp[38] == 0",  // ADS 返回码（4B，小端）首字节 0
  "dataStartIndex": 46,               // 数据区：返回码/长度之后
  "dataLengthExpr": ""
}
```

ADS Read 响应：`30..33` 是 AMS 错误码，`34..37` 是调用 ID，`38..41` 是 **ADS 返回码**，`42..45` 是实际长度，数据从 `46` 开始。

---

## 3. 逐字节示例

`StartByteAddress = 0x1A0`（416）、`ByteCount = 2`、缺省 NetId/端口、调用 ID 首帧 = 1：

```
00 00  2C 00 00 00  01 01 01 00  A8 C0  21 03  01 01 01 00  A8 C0  60 59
02 00  04 00  0C 00 00 00  00 00 00 00  01 00 00 00
01 F0 00 00  A0 01 00 00  02 00 00 00
```

50 字节，与 `22-14` … `22-19` 的断言一致（调用 ID 随每次请求递增）。

---

## 4. ⚠️ 接入前必须确认的两件事

1. **`IndexGroup = 61441` 即 `0xF001`**，而配置里的 label 写的是 `0xF005` —— 两者不是同一个值。按句柄读取（`0xF005`）、按名字读取或直接读存储区（如 M 区）用的 `IndexGroup` 各不相同，请按你的 TwinCAT 工程确认；该值可以在标签 `variables` 里覆盖，不必改协议文件。
2. **NetId**：缺省值是 `192.168.1.1.1.0` 的示例编码，几乎一定要按现场改（见 §2.1）。同一台 PLC 上多设备时用 `devices[].variables` 覆盖。

```jsonc
{
  "id": "PLC-ADS-1",
  "protocol": "twincat-ads",
  "connection": { "host": "192.168.1.30", "port": 48898 },
  "variables": { "NetIdHi": 49320, "NetIdLo": 16843009, "TargetPort": 801 }
}
```

---

## 5. 常见坑

| 现象 | 原因 | 处理 |
|------|------|------|
| 连接被对端断开 | AMS 长度写成整帧长 | `{Frame:fixed} - 6` |
| ADS 返回 `0x745`（端口未打开）| `TargetPort` 不是运行时端口 | 按 TwinCAT 系统服务配置改 |
| 返回 `0x70C` / 找不到符号 | `IndexGroup` / `IndexOffset` 不匹配寻址方式 | 确认按句柄/按偏移/按名字 |
| 只通一台设备 | NetId 用了缺省示例值 | 逐设备覆盖 `NetIdLo`/`NetIdHi` |
| 调用 ID 不递增 | 模板未引用 `{InvokeIDSeed:X8LE}` | 保留该占位符，由 `autoIncrement` 注入 |

---

## 6. 附录：证据清单（Test 22）

| 断言 | 内容 |
|------|------|
| `22-p3` / `22-14a` | 协议加载、请求构建成功 |
| `22-14` | 帧长 50 |
| `22-14b` | AMS 长度 `2C 00 00 00` |
| `22-15` | 目标 NetId / 端口（`01 01 01 00 / A8 C0 / 21 03`）|
| `22-16` / `22-17` / `22-17b` | 命令 `02 00`、状态 `04 00`、数据长 `0C 00 00 00` |
| `22-18` / `22-19` | IndexOffset `A0 01 00 00`、读取长度 `02 00 00 00` |
| `22-20` | 长度域 = 偏移 2 / 4 字节 / 小端 |

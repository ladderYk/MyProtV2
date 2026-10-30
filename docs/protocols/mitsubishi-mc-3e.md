# 三菱 MC 协议 3E 帧教学（`50 00` 副头部 + 小端长度域）

> 本文面向**已了解三菱 MC 协议（MELSEC Communication Protocol）3E 帧、想把 MyProt 配置映射到实际字节**的读者。
>
> **前置知识**：3E 帧 = 副头部 `50 00` + 访问路径（网络号/PC 号/请求目标）+ 请求数据长 + 监视定时器 + 命令 + 子命令 + 软元件地址 + 点数；设备作**服务端**（缺省端口 5007）。
>
> **关联文件**：
> - 协议配置：[configs/protocols/mitsubishi-mc-3e.json](../../configs/protocols/mitsubishi-mc-3e.json)
> - 配置 Schema：[docs/Config_Schema.md](../Config_Schema.md)
> - 姊妹文档：[omron-fins-tcp.md](./omron-fins-tcp.md)、[twincat-ads.md](./twincat-ads.md)
>
> **证据分级**：
>
> | 标记 | 含义 |
> |:--:|------|
> | ✅ | **仓库内已验证**：`src/Tests/E2EMain.cpp` 的 `Test 22` 有对应断言（22-7 … 22-13）|
> | 🧮 | **推导**：由模板逐字节数出，或由 ✅ 线性推出 |
> | ⚠️ | **待手册/真机核对**：本仓无真机，按公开手册整理 |

---

## 0. 30 秒速览

```jsonc
{
  "transport": "Tcp, defaultPort 5007",
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 7,          // 长度域在"请求数据长"处
    "lengthFieldLength": 2,
    "lengthIncludesHeader": false,
    "byteOrder": "LittleEndian",     // MC 3E 的长度域是小端
    "headerLength": 9                // 长度值 = 总帧长 - 9
  },
  "dataByteOrder": "WordLittleByteBig",
  "operations": "ReadDRegisters (0401 批量读 D 寄存器)"
}
```

| 层级 | 字节 |
|------|------|
| 副头部 + 访问路径（7B）| `50 00` + 网络号 + PC 号 + 请求目标(2+1) |
| 请求数据长（2B，LE）| 监视定时器起至帧尾 |
| 监视 + 命令 + 子命令（6B）| `Watchdog(2)` + `01 04` + `SubCmd(2)` |
| 软元件 + 点数（6B）| 设备码(1) + 头软元件(3, LE) + 点数(2, LE) |

---

## 1. 帧结构

### 1.1 布局（读 D 区，2 字）

```
偏移: 0  1 │ 2 │ 3 │ 4  5 │ 6 │ 7  8 │ 9 10 │11 12│13 14│15 │16 17 18│19 20│
内容: 50 00│Net│PC │目标IO│站 │数据长│监视  │01 04│子命令│A8 │头软元件│点数 │
      └副头┘└─ 访问路径 ─┘└─LE─┘└─LE─┘└命令┘└─LE─┘└设备码┘└─LE──┘└─LE─┘
```

| 偏移 | 字段 | 长度 | 本配置取值 | 说明 |
|:--:|------|:--:|------|------|
| 0..1 | 副头部 | 2 | `50 00` | 3E 帧（二进制）✅ 22-8 |
| 2 | 网络号 `NetNo` | 1 | `00` | 本网 |
| 3 | PC 号 `PCNo` | 1 | `FF` | 直接连接 |
| 4..5 | 请求目标模块 I/O | 2 | `00 00` | CPU |
| 6 | 请求目标站 `ReqDestSta` | 1 | `00` | |
| 7..8 | 请求数据长 `ReqDataLen` | 2 | `{Frame:fixed} - 9` | 小端；21B 帧 → `0C 00` ✅ 22-8b |
| 9..10 | 监视定时器 `Watchdog` | 2 | `1600` = `0x0640` → `40 06` | 250 ms 规格 ✅ 22-8c |
| 11..12 | 命令 | 2 | `01 04` | 批量读 ✅ 22-9 |
| 13..14 | 子命令 `SubCmd` | 2 | `00 00` | 字单位 |
| 15 | 设备码 `DevCodeD` | 1 | `A8` | D 寄存器 ✅ 22-10 |
| 16..18 | 头软元件 `HeadDev` | 3 | `StartByteAddress / 2` | 小端 3 字节 ✅ 22-11 |
| 19..20 | 点数 `Points` | 2 | `ByteCount / 2` | 小端 ✅ 22-12 |

### 1.2 长度域语义（★）

长度域在**偏移 7**、2 字节、**小端**、不含前 9 字节：

```
请求数据长 = 总帧长 - 9 = 21 - 9 = 12 (0x000C) → 线上 0C 00
```

模板里写成 `{Frame:fixed} - 9`，成帧层负责把它写到 7..8 并校验自洽（ADR-0012 试算）。

> **不要**把这个字段当成"整帧长度"：3E 的"请求数据长"从**监视定时器**开始计数。

---

## 2. 配置逐字段对照

### 2.1 设备与地址单位

```jsonc
{
  "id": "PLC-MC-1",
  "protocol": "mitsubishi-mc-3e",
  "connection": { "host": "192.168.1.20", "port": 5007 }
}
```

标签里仍然写**字节**地址：

```jsonc
{
  "name": "MC1.D50",
  "deviceId": "PLC-MC-1",
  "operation": "ReadDRegisters",
  "direction": "read",
  "scanRateMs": 1000,
  "finalType": "UInt16",
  "variables": { "StartByteAddress": 100, "ByteCount": 2 }   // D50 = 100 字节 / 2
}
```

### 2.2 inputs

| 名称 | 缺省 | 作用 |
|------|:--:|------|
| `StartByteAddress` / `ByteCount` | 0 / 2 | 引擎契约，字节单位 |
| `NetNo` / `PCNo` | 0 / 255 | 访问路径 |
| `ReqDestIO` / `ReqDestSta` | 0 / 0 | 目标模块 / 站 |
| `Watchdog` | 1600 | 0 = 不监视 |
| `DevCodeD` | `0xA8` | D 区；其他软元件改这里 |
| `SubCmd` | 0 | 字单位（位单位用 `0x0001`，需另配模板）|

### 2.3 outputs

| 名称 | 表达式 | 说明 |
|------|--------|------|
| `ReqDataLen` | `{Frame:fixed} - 9` | 请求数据长 |
| `HeadDev` | `StartByteAddress / 2` | 头软元件号（D 区字地址）|
| `Points` | `ByteCount / 2` | 点数（字）|

### 2.4 responseParser

```jsonc
{
  "validCondition": "resp[9] == 0",   // 响应码在 9..10，低字节 0 表示正常
  "dataStartIndex": 11,               // 数据从 11 开始
  "dataLengthExpr": ""                // 取到帧尾
}
```

响应帧的副头部是 `D0 00`，访问路径与请求同构，`9..10` 为响应码，数据紧随其后。

### 2.5 `dataByteOrder` 说明

`WordLittleByteBig` 的语义是**高字在前、字内小端**（多字数值按 BADC 拼装）。单字标签的线上字节序请按你设备/软元件的位宽与手册核对（⚠️ 无真机验证）。

---

## 3. 逐字节示例

`StartByteAddress = 100`（D50 = `0x32`）、`ByteCount = 4`（2 字）：

```
50 00  00 FF 00 00 00  0C 00  40 06  01 04  00 00  A8  32 00 00  02 00
└副头┘ └─ 访问路径 ─┘ └长┘  └监视┘ └命令┘ └子命令┘ └码┘ └头软元件┘ └点数┘
```

21 字节，与 `22-7` … `22-12` 的断言一致。

---

## 4. 常见坑

| 现象 | 原因 | 处理 |
|------|------|------|
| 设备回错误码 | 请求数据长写成整帧长 | 用 `{Frame:fixed} - 9`，小端 |
| 读到的字顺序颠倒 | 多字数值的 `dataByteOrder` 选错 | 按手册在 `LittleEndian` / `WordBigByteLittle` / `WordLittleByteBig` 间选择 |
| 地址偏移 2 倍 | `StartByteAddress` 填了字地址 | 填字节地址（D50 → 100）|
| 位读写失败 | 子命令/设备码仍按字配置 | 位单位需 `SubCmd = 0x0001` 并改模板 |
| 监视定时器超时 | `Watchdog` 太小 | 缺省 1600（250 ms），长帧适当加大 |

---

## 5. 附录：证据清单（Test 22）

| 断言 | 内容 |
|------|------|
| `22-p2` / `22-7a` | 协议加载、请求构建成功 |
| `22-7` | 帧长 21 |
| `22-8` / `22-8b` / `22-8c` | `50 00`、数据长 `0C 00`、监视 `40 06` |
| `22-9` / `22-10` | 命令 `01 04`、设备码 `A8` |
| `22-11` / `22-12` | 头软元件 `32 00 00`、点数 `02 00` |
| `22-13` | 长度域节点 = 偏移 7 + 小端 |

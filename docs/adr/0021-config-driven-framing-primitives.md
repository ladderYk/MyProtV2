# ADR-0021：配置驱动的帧规格原语（varint / 位域 / 分派 / 重复记录）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-25 |
| 日期 | 2026-10-25 |
| 相关文档 | [ADR-0020](./0020-frame-layer-vs-session.md) · [ADR-0002](./0002-transport-abstraction.md) · [Config_Schema §2.2](../Config_Schema.md#22-framing判别联合体判别字段-type) · [03_Transport §3.4](../modules/03_Transport.md) · [12_Southbound §12.2](../modules/12_Southbound.md) |
| 提案来源 | ADR-0020 把帧层独立出来之后，帧层仍只会「按定长长度域切一刀」，描述不了 MQTT 固定头这类真实线格式；「协议 = 可被配置描述的字节流」还差四个原语 |

---

## 背景

ADR-0020 定义了 `Core::Frame` / `Core::IFrameCodec`，把「字节 → 一帧」与「帧 → 会话动作」分开。但落地后的帧层只有两种能力：

| 能力 | P0 状态 |
|------|---------|
| 切帧 | `LengthField`（1/2/4 字节定长长度域）与 `Fixed` |
| 字段 | 无——上层拿到的是整帧字节，必须自己再按偏移切 |
| 报文类型 | 无——`Frame::type` 恒为 0 |
| 重复结构 | 无——表类载荷只能整体交给上层 |

这带来三个具体缺口：

1. **长度域编码**：MQTT 的 Remaining Length 是 base-128 varint（每字节低 7 位有效、最高位为继续标志），`lengthFieldLength ∈ {1,2,4}` 无法表达；`byteOrder` 也没有意义。
2. **头部位域**：MQTT 固定头首字节是「高 4 位 type + 低 4 位 flags」，Modbus 功能码、S7 头部的位标志同理。没有位域提取，会话层仍要碰字节布局，违背 ADR-0020 §2.1 的「会话代码不碰线格式」。
3. **载荷语义**：P0 的 `StreamFrameCodec` 把整帧（含帧头）作为 `Frame::payload`。若上层是 JSON 映射，载荷前的长度前缀会让解析必然失败；上层被迫知道 `headerLength`，帧层形同虚设。

P1 的目标不是「用配置描述一切协议」，而是补齐**让帧层对真实 TCP 协议有用**的最小原语集合。

## 决策

### 2.1 varint 长度域

`LengthFieldConfig` 新增：

| 字段 | 类型 | 默认 | 语义 |
|------|------|------|------|
| `lengthFieldEncoding` | `"Fixed"` / `"Varint"` | `"Fixed"` | 长度域自身的编码方式 |
| `varintMaxBytes` | int | 4 | Varint 的最大字节数，取值 1~4 |

规则：

- `Varint` 时 `lengthFieldLength` **必须为 0**（宽度运行时才知道），否则加载期 Error。
- 编码固定为 **小端 base-128**：`value |= (b & 0x7F) << (7 * i)`，`b & 0x80` 为继续标志；`byteOrder` 对 Varint 不生效。
- 超过 `varintMaxBytes` 仍未见到终止字节 → `Malformed`（防止无限累积）。
- `headerLength = 0` 时，帧头长度按 **`lengthFieldOffset + 实际 varint 宽度`** 计算；`headerLength > 0` 时仍以配置为准。
- `lengthIncludesHeader` / `lengthAdjustment` 语义与定长长度域完全一致。

### 2.2 位域：`headerFields`

```json
"headerFields": [
  { "name": "type",  "bitOffset": 0, "bitWidth": 4 },
  { "name": "flags", "bitOffset": 4, "bitWidth": 4 }
]
```

- `bitOffset` 从**帧首字节的最高位**开始计数（bit 0 = byte 0 的 bit 7），MSB-first。
- `bitWidth` 取值 1~32，结果按无符号大端解释后写入 `Core::Frame` 的同名 `UInt` 字段。
- 偏移是**帧内绝对偏移**（含帧头），因此位域可以落在长度域之前、之中或之后。
- 名称必须非空且在同一个 framing 内唯一；`bitOffset + bitWidth` 不得超过 `maxFrameSize * 8`。
- 未声明 `headerFields` 时，行为与 P0 完全一致。

> 位域只做「读出来」，不做写回；`Encode` 仍由 codec 自身实现（当前 `StreamFrameCodec::Encode` 为恒等）。

### 2.3 分派：`dispatch`

```json
"dispatch": {
  "field": "type",
  "rejectUnknown": true,
  "cases": [ { "value": 3, "type": 3, "name": "Publish" } ]
}
```

- `dispatch.field` 必须是 `headerFields` 中已声明的字段；未找到 → 加载期 Error。
- 命中 case：`Frame::type = case.type`（缺省 = `case.value`），`Frame::typeName = case.name`。
- 未命中且 `rejectUnknown = true`（默认）→ `Malformed`；`= false` → `Frame::type = 原始值`、`typeName` 为空。
- 同一 `value` 重复声明 → 加载期 Error；声明了 `cases` 却没有 `field` 同样 Error。
- `Frame::type` 只要求「协议内稳定」，不要求与线上数值相同——这样同一会话层可以复用于数值不同的厂商变体。

### 2.4 重复记录：`repeat`

```json
"repeat": { "countField": "count", "stride": 2, "offset": 3, "name": "records", "maxCount": 1024 }
```

- `countField` 必须是已声明位域，取值为记录条数；`stride` 为每条记录字节数（> 0）。
- 记录区 = `[offset, offset + count * stride)`，偏移相对**帧首**。
- 整段以 `Core::Frame` 的 `Bytes` 字段（`name`，默认 `records`）暴露；条数为 0 时给出空视图。
- `count > maxCount`，或 `offset + count * stride` 越过帧尾 → `Malformed`（不截断、不猜测）。
- 这是**载荷记录区**原语，不是 HTTP chunked：不支持可变长记录、不支持续块拼接。

### 2.5 payload 语义修正

| framing | `Frame::payload` |
|---------|------------------|
| `LengthField` | 帧头之后的全部字节：`headerLength > 0` 时从 `headerLength` 起；否则从 `lengthFieldOffset + 长度域实际宽度` 起 |
| `Fixed` | 整帧（无帧头概念，保持 P0 行为） |

理由：`Frame::payload` 的契约是「下一层要消费的区间」。帧层既然已经知道帧头边界，就不该把这个偏移再泄露给会话层/映射层。整帧仍然可以通过位域/记录区拿到需要的头部信息。

> **兼容性**：`lengthFieldEncoding` 默认 `Fixed`，未配置新字段的既有协议行为不变。`LengthField` 入站的 `payload` 由「整帧」改为「帧头之后」是有意修正——仓库内现有入站配置均使用 `Fixed`，不受影响；自定义 `LengthField` 入站若依赖旧语义，需在映射侧补回帧头（推荐改为声明位域）。

### 2.6 落地位置

| 位置 | 职责 |
|------|------|
| `Transport::LengthFieldFrameParser` | 切帧：定长 / varint 长度域、动态帧头长度、`maxFrameSize` 守门 |
| `Transport::StreamFrameCodec`（`Core::IFrameCodec`） | 位域提取 → 分派 → 重复记录 → payload 视图；`LengthField` 与 `Fixed` 的统一入口 |
| `Service::SchemaChecker` | 上述静态规则的加载期校验（§2.7） |
| `Service::ProtocolJsonParser` / `SchemaRegistry` | 字段解析与 Schema 字段表 |

不在 `MqttCodec` 内部单独扩 varint：MQTT 走自己的 `IFrameCodec` 实现，但长度域语义只保留 `LengthFieldFrameParser` 一份，避免两套 varint 再次分叉。

## 校验规则（加载期）

| 规则 | 结果 |
|------|------|
| `Varint` + `lengthFieldLength != 0` | Error |
| `varintMaxBytes ∉ [1,4]` | Error |
| `headerFields[].name` 为空 / 重复 | Error |
| `bitOffset < 0` / `bitWidth ∉ [1,32]` / 越出 `maxFrameSize*8` | Error |
| `dispatch.field` 未在 `headerFields` 声明 | Error |
| `dispatch.cases[].value` 重复 / 有 cases 无 field | Error |
| `repeat.countField` 未在 `headerFields` 声明 | Error |
| `repeat.stride <= 0` / `offset < 0` / `maxCount <= 0` / `name` 为空 | Error |
| 有 `repeat.stride` 无 `countField` | Error |

运行期只保留两类失败：数据不足（`NeedMore`）与规格违例（`Malformed`，调用方断链）。不做「尽量解析」。

## 被否决的方案

| 方案 | 否决理由 |
|------|----------|
| 用正则/脚本描述帧 | 需要嵌入式解释器；C++11/VS2015 无现成依赖；每帧解释成本高，且正则难以表达「长度域 + 位域 + 记录区」的组合 |
| 设计一套完整二进制模板 DSL | 当前只有 MQTT / TCP 入站两个真实用例，原语集合会在没有第二个消费者时固化；先补最小集，按触发条件扩展 |
| varint 只做在 `MqttCodec` 内 | MQTT 能用，但 TCP 入站仍描述不了 varint 协议；两套切帧继续分叉（正是 ADR-0020 要消除的） |
| 保持 `payload = 整帧`，把帧头偏移交给上层 | 上层必须知道线格式，ADR-0020 的「会话不碰字节布局」失效；JSON 映射必须先剥前缀，配置复杂度转移到代码 |
| 用 `repeat` 兼做 HTTP chunked | chunked 需要「读长度 → 读块 → 再读长度」的迭代状态，属于会话层循环；`repeat` 只处理帧内定长记录，混用会让两个概念都表达不清 |

## 后果

- **正面**：MQTT 固定头、varint 长度、TLV 表都能用同一套配置表达；`StreamFrameCodec` 不再是「只会切一刀」的占位实现；非法规格在加载期暴露，而不是在设备连上来时才发现。
- **代价**：`LengthFieldConfig` 从 7 个字段增长到 12 个（含 3 个嵌套对象），配置面变大；文档与校验规则必须同步维护。
- **明确不做**：校验和/CRC 原语、可变长记录、chunked 续块、嵌套 TLV、由规格反向编码（`Encode` 仍恒等）。这些在出现第二个真实用例时单独立项。

## 复审触发条件

1. 出现 ≥ 2 个协议需要「可变长记录 / 嵌套 TLV」；
2. 需要把管理面 HTTP（chunked）也接入帧层；
3. 需要由配置生成请求字节（反向 `Encode`），而不只是解析；
4. `headerFields` 数量超过 ~16 个，线性查找成为热点。

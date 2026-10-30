# ADR-0020：帧层与会话层分离（`Core::IFrameCodec`）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-25 |
| 日期 | 2026-10-25 |
| 相关文档 | [02_Layered_Architecture §2](../architecture/02_Layered_Architecture.md) · [03_Threading_and_DataFlow](../architecture/03_Threading_and_DataFlow.md) · [11_Northbound §11.2](../modules/11_Northbound.md) · [12_Southbound §12.2](../modules/12_Southbound.md) · [ADR-0002](./0002-transport-abstraction.md) · [ADR-0019](./0019-southbound-push-ingest.md) |
| 提案来源 | 「用描述字节流的方式表达一切协议」这一设计主张在代码里缺少落点：MQTT 与 TCP 入站各自维护一套切帧逻辑，会话状态又混在 IO 对象中；新增协议只能再抄一遍 |

---

## 背景

项目的核心主张是**协议 = 可被配置描述的字节流**。但 MQTT 与 HTTP 虽然都跑在 TCP 上，落到代码里却无法复用同一条链路，原因是把三件不同的事压在了一起：

| 层 | 职责 | 状态性 | 能否数据驱动 |
|----|------|--------|-------------|
| L1 传输 | 谁连谁、字节怎么来 | 有（连接/缓冲） | 否 |
| L2 帧与字段 | 从字节流切出一帧，取出字段 | **无**（纯函数） | **能**（文法可描述） |
| L3 会话 | 收到 CONNACK 该做什么、何时重连、订阅表 | 有（状态机） | 否 |

现状是 L2 出现三份平行实现：

1. `Transport::IFrameParser` / `LengthFieldFrameParser`：只回答「这一帧到哪结束」，返回 `std::vector<uint8_t>` 整帧拷贝，**没有字段提取**；
2. `Northbound::MqttCodec::TryDecode`：自己切固定头 + 解码 varint 剩余长度，字段解析另走 `ParseConnack/ParseSuback/ParsePublish`；
3. `WebApi::WebApiServer`：按 `Content-Length` 切 HTTP 报文，只支持定长。

同时 `MqttClient` 一个类里同时承担 socket 读写、发送队列、心跳定时器、订阅表、包序号、CONNACK/SUBACK/PUBACK 语义，约 500 行，任何一处改动都要通读全文。

`LengthFieldConfig.lengthFieldLength` 只支持 1/2/4 字节定长，**描述不了 MQTT 的 base-128 varint**——这正是「配置描述字节流」主张的第一个缺口。

## 决策

### 2.1 统一帧抽象：`Core::Frame` + `Core::IFrameCodec`

```cpp
enum class DecodeStatus { Complete, NeedMore, Malformed };

class IFrameCodec {
public:
    virtual ~IFrameCodec() {}
    virtual DecodeStatus Decode(const ByteView& buf, std::size_t& consumed, Frame& out) = 0;
    virtual bool Encode(const Frame& frame, std::vector<uint8_t>& out) = 0;
};
```

`Frame` = `type`（协议内报文类型）+ `flags`（同层标志位）+ `payload`（`ByteView`）+ 一张 `name → FrameField` 字段表。`FrameField` 是 tagged struct（`UInt/Int/Bool/String/Bytes`），不用 `std::variant`——C++11 没有。

三条硬约束：

- **零拷贝**：`Frame::payload` 是指向调用方收流缓冲区的视图，只在**下一次 `Decode` 之前**有效；`Frame` 显式禁止拷贝，避免浅拷贝出的悬垂视图被误存。
- **三态返回**：`Complete`（`consumed` 报告消费字节数）/ `NeedMore`（缓冲区不足，不消费）/ `Malformed`（线格式非法，调用方断链）。
- **`consumed` 由 codec 报告**：调用方不做任何长度推算，跨包/粘包只由一个循环处理。

### 2.2 三段职责与当前落地

| 层 | 类 | 内容 |
|----|----|------|
| L2 帧 | `MqttCodec : Core::IFrameCodec` | 固定头 + varint 剩余长度 + 字段填充；旧静态接口（`EncodePublish` / `TryDecode` / `Parse*`）全部保留 |
| L3 会话 | `MqttSession` | CONNECT/PUBLISH/SUBSCRIBE/PINGREQ 构造、CONNACK/SUBACK/PUBACK/PINGRESP/PUBLISH 语义、订阅表与包序号；**不持有 socket、不开线程** |
| L1 传输 | `MqttClient` | socket、读写缓冲、发送队列、心跳与重连定时器、指数退避 |

`MqttClient::ProcessRx` 只剩一个解码循环：

```cpp
while (offset < _rxBuf.size()) {
    Core::Frame frame;
    std::size_t consumed = 0;
    const Core::DecodeStatus st = _codec->Decode(ByteView(&_rxBuf[0] + offset, ...), consumed, frame);
    if (st == NeedMore) break;
    if (st == Malformed) { FailAndReconnect("malformed packet"); return false; }
    offset += consumed;
    if (!HandleFrame(frame)) return false;   // 转交 _session.OnFrame
}
```

`MqttSession::OnFrame` 返回 `Event{None, Connected, Fatal}`，由传输层决定「继续读」还是「重连」——会话层不直接碰 socket。

**行为等价以既有断言为准**：`MqttCodec` 的 13 个既有用例、102 条断言零修改通过；`MqttClient` 的连接后动作顺序（ArmKeepAlive → 重订阅 → `_onConnected`）与错误文案逐字保留。

### 2.3 栈底统一：`Transport::StreamFrameCodec`

`TcpIngestServer` 原先直接持有 `IFrameParser`，现在统一为：

```
TcpIngestServer → Core::IFrameCodec
                    ├── StreamFrameCodec（LengthField / Fixed → 不透明帧，Encode 恒等）
                    └── MqttCodec（自有线格式）
```

`StreamFrameCodec` 是帧栈的栈底实现：只负责切出一帧并（可选）提取字段，不解释业务内容。P0 时整帧直接成为 `Frame::payload`；[ADR-0021](./0021-config-driven-framing-primitives.md) 之后 `LengthField` 的 payload 改为帧头之后的应用字节（`Fixed` 仍为整帧）。TCP 入站的 `PayloadMapper` 仍然自己解析 JSON——**帧层只负责切分与字段提取，不做业务解释**。

### 2.4 四个原语（P1，已由 ADR-0021 落地）

当前 `IFrameCodec` 接口就位后，要让「配置描述帧」成立，还缺四个原语；P1 已在 [ADR-0021](./0021-config-driven-framing-primitives.md) 中实现前四项（`repeat` 仅限帧内定长记录，不含 chunked）：

| 原语 | 解锁 | ADR-0021 落地 |
|------|------|------|
| `varint`（base-128 长度） | MQTT 剩余长度、protobuf、部分私有协议 | `lengthFieldEncoding=Varint` + `varintMaxBytes` |
| `bits`（位域字段） | Modbus 线圈打包、S7 位地址、CAN 信号 | `headerFields[{name,bitOffset,bitWidth}]` |
| `dispatch`（按首字节分派报文类型） | MQTT 固定头、IEC104 ASDU、私有 TLV | `dispatch{field,cases[],rejectUnknown}` |
| `repeat`（帧内重复记录） | 批量记录 / 表类载荷 | `repeat{countField,stride,offset,maxCount}` |

这四项之外（可变长记录、chunked 续块、CRC、反向编码），**新协议仍应手写 `IFrameCodec` 实现**，不要假装配置能覆盖。

### 2.5 边界：管理面 HTTP 保持手写

`WebApiServer` 不接入 `IFrameCodec`：

- 它需要长连接、SSE 流式响应、chunked 请求体，不是「一帧一响应」语义；
- 管理面生命周期独立于设备协议栈，接入后会把配置面与设备面绑死；
- 收益（省几百行）小于风险（管理面回归）。

结论：**帧抽象服务于设备协议栈，不追求「全项目只有一个解析器」**。

## 备选方案与放弃理由

| 方案 | 放弃理由 |
|------|----------|
| 继续用 `IFrameParser` 并加一个 `ParseFields` | 返回整帧拷贝，字段提取仍要二次扫描；且接口一旦加方法，所有既有实现都要改 |
| 把一切帧写进 JSON 配置模板（纯数据驱动） | 单条报文是正则语言可以描述，**跨报文是状态机**（CONNACK 失败要断链、QoS1 要回 PUBACK），配置无法表达；强推会让配置变成图灵完备的脚本 |
| `MqttClient` 继续自己解析，只抽接口做样子 | 接口不进热路径就是死接口；本次让 `Encode(CONNECT)` 与 `Decode` 都走通用路径，并用往返测试锁死对称性 |
| 引入 `std::variant` / 第三方 codec 库 | 工具链锁 VS2015(v140)/C++11（ADR-0010），无 `variant`；第三方库无法表达项目自定义帧语义 |

## 后果

**正面**

- 新增 TCP 系协议只需实现一个 `IFrameCodec`，`ProcessRx`/缓冲/粘包逻辑零重复；
- `MqttSession` 可脱离 socket 单测：构造 `Frame` 即可验证 CONNACK 拒绝、QoS1 回执、重连后重放订阅；
- `Frame` 的字段名是字符串，日志与测试可直接按名断言，不必记忆字节偏移；
- 帧层无状态，天然线程安全；`payload` 视图生命周期规则由 `Frame` 的不可拷贝强制。

**代价 / 约束**

- `Frame::payload` 是视图：调用方若需跨帧保存，必须自行拷贝（`MqttSession` 在投递消息前已转成 `std::string`）；
- 字段表用 `std::vector<pair<string, FrameField>>` 线性查找：单帧字段数是个位数，可接受；超过 16 个字段再考虑改 map；
- `MqttCodec` 现在有两套入口（旧静态接口 + `IFrameCodec`），新增报文必须同时补两处，否则旧测试与运行时会分叉；
- 配置侧已由 ADR-0021 补上 varint/位域/分派/帧内重复记录；但可变长记录、chunked、CRC 与反向编码仍不在配置能力内。

## 复审触发条件

1. 出现**第三个**协议需要同一套切帧循环（当前 MQTT + 长度字段/TCP 两条）→ 评估把 `ProcessRx` 循环上提为 `Core::FrameReader` 模板；
2. 需要 `varint` / `bits` / `dispatch` / `repeat` 之外的新原语（可变长记录、chunked、CRC、反向编码）→ 另立 ADR 定义配置文法，并同步 `Config_Schema §2.2 framing`；
3. 单帧字段数普遍超过 16 → `Frame` 字段表改 `std::map` 或加编译期固定槽位；
4. 需要在回调之外**跨帧**持有 `payload` → 给 `Frame` 增加可选拥有语义（`shared_ptr<const std::vector<uint8_t>>`），并明确拷贝代价；
5. 管理面需要 chunked 请求体 → 先补 `repeat` 原语，再评估是否复用 `StreamFrameCodec`。

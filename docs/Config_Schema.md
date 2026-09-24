# MyProtV2 — 配置 Schema 定稿

> **文档地位**: 本文档是配置契约的**唯一事实来源 (Single Source of Truth)**——若 `modules/01_Core.md` 的 POCO、`modules/04_Service.md` 的加载校验、`modules/02_Engine.md` 的模板解析与本文档冲突，以本文档为准，相应模块文档需同步修订。

---

## 0. 约定
1. **JSON 字段 == C++ POCO 字段**，统一小驼峰，借 nlohmann/json 的 `adl_serializer` 直接映射，不允许出现 JSON 与结构体字段名不一致的情况。
2. 所有枚举值在 JSON 中以字符串表示（如 `"BigEndian"`、`"OnChange"`），大小写敏感。
3. "可选"字段缺省时取文中给出的默认值；加载器在缺省时不得报错。
4. 时间单位统一为毫秒（字段名以 `Ms` 结尾）。
5. **配置代际 `schemaVersion`**（整数，裁决见 [ADR-0005](./adr/0005-config-versioning.md)）：标识配置格式的机器代际，当前 `kSupportedSchemaVersion = 2`。它与本文档修订号（v4.0 等）**解耦**——文档小步修订不改代际，仅破坏性格式变更才递增代际。`ConfigRoot` 与每个 `ProtocolConfig` 顶层均可选携带；缺省 = Warning + 假定当前代际，存在但不匹配 = 启动 Fail-Fast（见 §7 版本门禁）。

---

## 1. 配置文件布局
```
configs/
├── protocols/            # 每个文件一份 ProtocolConfig（可选顶层 schemaVersion）
│  ├── modbus-tcp.json
│  └── s7-1200.json
└── tags.json             # ConfigRoot { schemaVersion?, resilience?, webApi?, devices[], tags[] }
```

大型部署可将 `tags.json` 拆分为 `devices.json` + `tags.json` 两个文件；`JsonConfigLoader::LoadConfigRoot` 检测到 `devices.json` 存在时分别加载并合并为 `ConfigRoot`。两种布局二选一，不允许三种文件同时存在。

**配置代际**（[ADR-0005](./adr/0005-config-versioning.md)）：`schemaVersion` 为整数代际号，`ConfigRoot` 顶层可选携带（拆分布局下写在 `tags.json`），每个协议文件顶层亦可选携带。加载器在字段级校验**之前**先做版本门禁：缺省 = Warning + 假定当前代际 `2`；存在但不等于 `kSupportedSchemaVersion` = `ConfigError` Fail-Fast；协议文件与根不一致 = `ConfigError`。

**目录布局**：

```
configs/
├── protocols/      # 一文件一协议 (例: modbus-tcp.json)
├── tags.json       # 设备 + 标签 + 全局韧性 + WebApi
└── server.json     # 全局服务端配置 (simulation)
                    # 可选缺失 — listenPort 默认 0 = 关闭仿真
```

`server.json` 的 `simulation` 段承载 listenPort / registerCount / initialValues / operations，与协议语法解耦。多个协议共享同一 `ServerConfig.simulation`。

---

## 2. 协议配置 (ProtocolConfig)

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `protocolName` | string | ✓ | — | 全局唯一，被 `DeviceConfig.protocol` 引用 |
| `transport` | object | ✓ | — | 见 §2.1 |
| `framing` | object | ✓ | — | 见 §2.2 |
| `dataByteOrder` | ByteOrder | ✗ | `"BigEndian"` | 协议级数据解码字节序；标签 `byteOrder` 未声明时回退至此（与 `framing.byteOrder` 长度域字节序解耦） |
| `operations` | map\<string, OperationConfig\> | ✓ | — | 非空，**map 的 key 即操作名**。写能力不在协议层声明（已移除 `writeOperation` / `writeBytesOperation` 协议级字段）——操作仅以 `kind: "read"/"write"` 标注语义，供校验器一致性核对与 UI 过滤；写声明点在标签层（见 §标签） |
| `inputs` | map\<string, VariableConfig\> | ✗ | `{}` | 协议级**输入段**：声明操作者提供 / 运行时生成的输入 —— `source=static`（含 `value` 注入默认值；无 `value` 仅作 UI 提示）与 `source=auto`（`strategy` 指定 `autoIncrement`/`frameSlice`/`expr`/`crc`，渲染期求值）。变长写载荷由模板 `{Name:raw}` 占位符自动识别，无需特殊策略声明。完整 `VariableConfig` 结构见 §3.2 |
| `outputs` | map\<string, VariableConfig\> | ✗ | `{}` | 协议级**输出段**。仅 `source=auto strategy=derivedLength` 的**派生输出**（按载荷字节数计算，见 §3.2.1）。`expr` 可引用 `inputs` 中已声明名或模板 `{Name:raw}` 载荷名，`{name:len}` 取字节长度（raw 载荷 = 实际字节数，其余 = 模板渲染宽度）。同一协议域内 `inputs` 与 `outputs` 不得同名（校验 Error）；跨操作允许同名不同角色（如 `RegisterCount` 读操作 = 输入 / 写操作 = 输出） |
| `handshake` | HandshakeStep[] | ✗ | `[]` | 空数组 = 无握手（如 Modbus） |
| `schemaVersion` | int | ✗ | 当前代际 | 配置代际号（§0 约定 / [ADR-0005](./adr/0005-config-versioning.md)）；缺省 = Warning + 假定当前代际，存在则须等于 `kSupportedSchemaVersion` 且与配置根一致 |
| `variableAliases` | map\<string, string\> | ✗ | `{}` | 变量别名映射（`别名 → 契约名`），让协议作者用自己的名字书写模板与变量表，引擎内部仍按契约名工作。详见下节 |
| `maxSpanBytes` | int | ✗ | `250` | 标签按地址邻近合并的最大跨度（**字节**），供 `TagGrouper::CoalesceAdjacent` 限制单次批读跨度 —— 即协议族的"单次读取上限"。Modbus 应取 `250`（FC03 上限 125 寄存器 × 2 字节）。须 > 0 |

> 下划线前缀字段（如 `_provenance`）被解析器忽略，不进 POCO、不参与校验，专用于书写机器无关的备注——约定来源说明（公开规范名称 / 抓包记录 / 手册版本）、商标声明等。建议每个协议文件携带 `_provenance` 字段留痕协议约定的独立推导来源（权属与 clean-room 证据）。
>
> 协议层**不含**仿真配置（`listenPort` / `initialValues` / `packetLossRate` 属服务端行为）；仿真段位于 `server.json` 的 `ServerConfig.simulation`。

**`variableAliases`（变量别名映射）**

> 协议 JSON 顶层声明 `别名 → 契约名`（契约名即引擎内部读取的名字，POCO 字段为 `varAliasMap`），即可在 `inputs` / `outputs` / 标签 `variables` / `requestTemplate` 中全程使用自定义名：

```jsonc
"variableAliases": { "SBA": "StartByteAddress", "BC": "ByteCount" }
```

| 项 | 规则 |
|---|---|
| **可映射目标（值）** | **仅 2 个**：`StartByteAddress` / `ByteCount` —— 引擎真正查表读取的**跨协议字节单位**。取其他名 = Error |
| **别名（键）** | 须为合法标识符（字母开头）；不得与内部名相同（无意义）= Error |
| **保留名冲突** | 不得占用 `Frame` / `__frameLen` / `__frameEnd` / `len` / `offset` / `fixed` = Error（它们分别是模板原语、expr 魔法变量、`{Name:prop}` 属性名） |
| **与变量表冲突** | 不得与同协议 `inputs` / `outputs` 中已有键重名 = Error（防止别名与同名变量并存） |
| **作用域** | 协议级。设备与标签不各自声明别名表，而是按 `deviceId → device.protocol` 沿用所属协议的映射 |

**归一化时机**：别名解析**不在运行期逐点查表**，而是在 `ConfigDirectoryLoader::ApplyVariableAliases` 中**一次性改写成内部名**；此后引擎各消费点无需感知别名的存在。

理由是那两处最核心的消费点**签名里没有 `protocol`** —— `TagGrouper::GetStartAddress(tag)` 与 `ResponseParser::Parse(response, config, tag, byteOrder)` —— 拿不到别名表；若要逐点透传，就得把名字解析散进整条调用链，制造新的"真源分裂"。

改写范围：

| 侧 | 改写内容 |
|---|---|
| 协议 | `inputs` / `outputs` 的**键** + 每项 `expr` 的**标识符**；`operations[].inputs` / `.outputs` 同上；`operations[].requestTemplate` 与 `handshake[].requestTemplate` 的 `{Name:...}` **占位符** |
| 设备 | `devices[].variables` 的键 |
| 标签 | `tags[].variables` / `tags[].writeVariables` 的键 |

> 两类文本的改写粒度不同，不可互换：**模板行**只改 `{Name:...}` 占位符（若连裸标识符一起改，会误伤 hex 字面量中的 `A`–`F`）；**`expr`** 只改裸标识符。

**与管理面解耦**：`ConfigStore` 以 **JSON 文本**为操作边界，不经过本函数，因此**管理面上用户看到的始终是自己的自定义名**，不会出现"存下去是别名、读回来变内部名"的往返污染。

**尚无实现的旁路**：加载器**只读协议文件内的 `variableAliases` 段**；曾设想的全局 `variable-aliases.json` 文件未实装，勿按此配置。

### 2.1 transport（判别联合体，判别字段 `type`）

**Tcp**

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"Tcp"` |
| `defaultPort` | uint16 | 502 | 设备未指定端口时的回退值 |

**Tls**（在 Tcp 基础上增加）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"Tls"` |
| `defaultPort` | uint16 | 0 | 0 表示必须由设备显式指定 |
| `certFile` / `keyFile` | string | — | 客户端证书/私钥路径，空 = 不使用双向认证 |
| `caFile` | string | — | CA 证书路径 |
| `verifyServer` | bool | true | 是否校验服务端证书 |

**Serial**

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"Serial"` |
| `portName` | string | — | 协议级默认串口名，可被设备级覆盖 |
| `baudRate` | uint32 | 9600 | |
| `dataBits` | uint8 | 8 | |
| `parity` | string | `"None"` | `None` / `Odd` / `Even` |
| `stopBits` | string | `"One"` | `One` / `Two` |

### 2.2 framing（判别联合体，判别字段 `type`）

> framing 共三分类（裁决见 [ADR-0002](./adr/0002-transport-abstraction.md)）：`LengthField` 为字节流成帧；`Silence` 为串口 RTU 静默成帧。`Fixed` 与 `Message`（CAN 预留）当前均未实装，见 [ROADMAP.md](./ROADMAP.md)。

**LengthField**

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"LengthField"` |
| `lengthFieldOffset` | int | 0 | 长度字段在帧中的字节偏移 |
| `lengthFieldLength` | int | — | 长度字段自身字节数，取值 1 / 2 / 4 |
| `lengthIncludesHeader` | bool | false | 长度值是否包含帧头自身 |
| `byteOrder` | ByteOrder | `"BigEndian"` | 长度字段的字节序 |
| `headerLength` | int | 0 | **帧头字节数**（帧首至长度字段结束）。总帧长公式：`lengthIncludesHeader = false` 时 `总帧长 = headerLength + 长度字段值 + lengthAdjustment`；`= true` 时 `总帧长 = 长度字段值 + lengthAdjustment`。**注意**：`0` 表示"长度字段前无帧头"，仅当长度字段恰在帧首时正确——如 Modbus MBAP（事务 ID 2B + 协议 ID 2B + 长度 2B = 6B 帧头，长度值为"其后字节数"）必须显式配 `6`，配 `0` 会切出短帧导致对端匹配失败 |
| `lengthAdjustment` | int | 0 | `总帧长 = 解析值 + lengthAdjustment`（±修正，如含 CRC 尾） |
| `maxFrameSize` | int | 1024 | 帧长安全上限，超出即判为非法 |

**Fixed**

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"Fixed"` |
| `fixedLength` | int | — | 固定帧长，必须 > 0 |

**Silence**（串口 RTU 静默成帧，v1）

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `type` | string | — | 固定 `"Silence"` |
| `charTimeUs` | int | 0 | 单字符时间（微秒）；0 = 按协议级波特率+数据位自动折算 |
| `frameGapUs` | int | 0 | 帧间静默阈值（微秒）；0 = `3.5 × charTimeUs`（Modbus RTU 约定） |
| `maxFrameSize` | int | 256 | 帧长安全上限，超出即判为非法 |

### 2.3 OperationConfig（operations 的 value）

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `requestTemplate` | string[] | ✓ | — | 非空，文法见 §3 |
| `responseParser` | object | ✓ | — | 见 §2.4 |
| `inputs` | map\<string, VariableConfig\> | ✗ | `{}` | 操作级输入覆盖：`source=static` 覆盖协议级默认值（含 `value` 才注入；无 `value` 即原 `hint` 语义）、`source=auto` 追加/覆盖协议级非 length 策略。与协议级 `inputs` 按**整条声明就近覆盖**（§3.4），schema 见 §3.2 |
| `outputs` | map\<string, VariableConfig\> | ✗ | `{}` | 操作级派生输出（`derivedLength`）。覆盖协议级同名 `outputs`；`expr` 引用域 = 本操作 `inputs` ∪ 协议级 `inputs`，`{name:len}` 取字节长度（载荷变量 = 实际字节数，其余 = 模板渲染宽度）。同一操作域内 `inputs` 与 `outputs` 不得同名（校验 Error） |
| `kind` | string | ✗ | `""` | 操作语义标注：`"read"` / `"write"`（可选，空 = 未标注，仅 UI 表单过滤与校验用） |

> 操作名不写在结构体内，由 map 的 key 给出；加载器负责把 key 回填到运行时对象的 `name` 字段。

> **超时归属（2026-08-24 收敛）**：单次请求/应答看门狗不再配置在操作上——统一由 **§4 `device.requestTimeoutMs`**（默认 3000）给出，该设备所有操作共用。语义不变：它是**故障检测看门狗**（单次尝试多久没回就判死），**不是期望延迟**；一次逻辑读取的总预算（deadline）由 §11 韧性策略给出，第 k 次尝试的有效超时 = `min(requestTimeoutMs, 剩余预算)`。健康路径延迟（P50/P99）仅统计成功且无重试的单物理请求，与本看门狗正交。旧配置中的 `operations[].timeoutMs` 字段会被忽略，见 [ADR-0004](./adr/0004-timeout-retry-budget.md) R1。

### 2.4 ResponseParserConfig

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `validCondition` | string | `""` | 表达式（文法见 modules/02 §2.1）；空 = 跳过校验。**注意**：引擎仅支持 `resp[N]==V` 单条件形式（如 `resp[7]==0x03`），多条件（`&&`/`||`/`!=` 等）**静默失效**——校验器加载期不检查，标签 AlwBad 且无错误提示。建议未来在 ConfigDeepValidator 增加语法校验。 |
| `dataStartIndex` | int | 0 | 数据区起始字节偏移 |
| `dataLengthExpr` | string | `""` | 数据区长度表达式（如 `"resp[8]"`）；空 = 取 `帧长 - dataStartIndex`。**注意**：此回退逻辑在真实采集路径与仿真路径均生效（`ResponseParser.cpp:160`），非"仿真专用"。 |

解析器只输出原始字节，不做类型转换。最终类型转换由 TagReader 依据 `TagDefinition.finalType` 在 ResponseParser::Parse 内部按字节序直接调用 `Core::FromBytesU16/U32/U64` 等装配函数完成。解析器不设独立的类型转换工具函数，也不设 `dataType` / `valueType` 字段。

#### 2.4.1 空值与异常语义（v1）

> **无独立"空值"概念**。`TypedValue` 9 种值类型（`UInt16/UInt32/UInt64/Int16/Int32/Int64/Float/Double/Bool/String/ByteArray`）不携带 `null` / `empty` 标记；值 `0`（数值）/ `""`（字符串）即"空"。

| 场景 | 实态行为 | 错误码 | 位置 |
|------|----------|--------|------|
| `validCondition` 空 / 缺省 | 跳过校验（视为通过） | 无 | [ResponseParser.cpp L137](src/Engine/src/ResponseParser.cpp#L137) `if (validCondition.empty()) return true;` |
| `validCondition` 解析失败（语法错 / `resp[]` 越界） | 视为不通过 | `InvalidResponse` | [L146-150](src/Engine/src/ResponseParser.cpp#L146-L150) |
| `dataStartIndex` 越界（< 0 或 > 帧长） | 解析失败 | `ParseError` | [L153-156](src/Engine/src/ResponseParser.cpp#L153-L156) |
| `dataLengthExpr` 空 | 回退到 `rawLen = 帧长 - dataStartIndex`（取剩余全量） | 无 | [L160-163](src/Engine/src/ResponseParser.cpp#L160-L160) |
| `registerCount = 0` 或 `dataStartIndex + rawLen` 越界 | 同上：回退到"取剩余全量" | 无 | [L160-163](src/Engine/src/ResponseParser.cpp#L160-L163) |
| `finalType` 未知 / 数据字节不足 | 解析失败 | `TypeConversionError` | [L172-176](src/Engine/src/ResponseParser.cpp#L172-L176) |
| 解析成功但值 = 0（数值） / `""`（字符串） | **正常 Good 质量** | 无 | [L178](src/Engine/src/ResponseParser.cpp#L178) `tv.quality = Good;` |

**契约要点**：
1. **"空" = 解析错误**（非"空值"）：设备返回 0 长度数据区 / 字节不足 / `finalType` 失配 → 走错误码路径，调用方拿到 `Unexpected`，标签批内对应位置标 `Bad`（[PollingEngine.cpp L228-238](src/Polling/src/PollingEngine.cpp#L228-L238)）。
2. **协议层零值与"无值"无区分**：`0` 与"寄存器未被使用"协议层无法区分，下游需自行通过 `validCondition` 或 `deadband` 过滤；如业务需"真 null"语义，应通过 `validCondition` 把 0 值映射为 `InvalidResponse`。
3. **质量码语义**：`Good` = 解析成功 + 字节完整 + 类型匹配；`Bad` = 任何解析失败；`Uncertain` 由 `PollingEngine::ResultDispatch` 回调依据 ADR-0006 推导（仅来自 `InvalidResponse` / `TypeConversionError` 错误码映射）。
4. **可观测性**：解析失败次数由 `myprot_parse_errors_total{device, operation, error_code}` 暴露（待 MetricsRegistry 实现，见 [ROADMAP.md](./ROADMAP.md)）。

### 2.5 HandshakeStep

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `name` | string | ✓ | — | 步骤名（日志与错误上下文用） |
| `requestTemplate` | string[] | ✓ | — | 见 §3 |
| `framingOverride` | FramingConfig | ✗ | null | 该步独立帧格式；null = 用协议全局 `framing` |
| `validCondition` | string | ✗ | `""` | 成功条件表达式 |
| `sessionExtractExpr` | string | ✗ | `""` | 会话变量提取表达式，如 `"resp[5:9]"` |
| `sessionVariable` | string | ✗ | `""` | 提取结果存入的会话变量名；与上一字段**必须成对出现** |
| `timeoutMs` | int | ✗ | 0 | 0 = 继承 `device.connection.timeoutMs` |

### 2.6 SimulationConfig（已迁服务端层）

> `SimulationConfig` 不属于 `ProtocolConfig`。仿真端（listenPort / registerCount / initialValues / operations）位于 [`server.json`](#13-服务端配置-serverconfig) 的 `ServerConfig.simulation`，与协议语法解耦。本节给出 schema 字段参考；实装位置与生效路径见 §13。

**SimOperationConfig** 字段表（`server.json` 内 schema 与之一致）：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `kind` | string | ✓ | — | `"read"`：按匹配出的地址/数量变量从数据区取数拼入应答；`"write"`：把请求数据区（`dataOffset` 起）写入数据区，应答不带数据 |
| `addressVar` | string | ✓ | — | 匹配变量名 → 数据区起始地址（须为该操作 `requestTemplate` 中出现的占位符名） |
| `countVar` | string | read 时必填 | `""` | read：匹配变量名 → 寄存器数量（应答数据字节 = count × 2 字节） |
| `dataOffset` | int | write 建议声明 | -1 | write：请求数据区起始偏移（帧内字节下标）；`-1` = 未声明（忽略写入） |
| `responseTemplate` | array&lt;string&gt; | ✗ | `[]` | 自定义应答模板；**非空时覆盖默认 echo 反向合成**。行文法：hex 字面量 / `{req:N:M}` 请求回显 / `{data}` 数据区；长度字段仍按 framing 自动重算（见 [modules/10 §10.6](./modules/10_Simulation.md)） |

---

## 3. 请求模板文法（定稿）

`requestTemplate` 是一个字符串数组，**每个元素**是且仅是以下两种形式之一：

### 3.1 十六进制字面量

空格分隔的字节序列，每字节两位十六进制：

```
"03 00 00 16"       → { 0x03, 0x00, 0x00, 0x16 }
"03"                → { 0x03 }
```

正则：`^([0-9A-Fa-f]{2})(\s+[0-9A-Fa-f]{2})*$`。不再支持旧示例中 `"0x03"` 这种独立写法（见 §8 迁移表）。

### 3.2 占位符

```
{名称}  {名称:格式}  {名称:raw}
```

| 成分 | 取值 | 说明 |
|------|------|------|
| 名称 | `[A-Za-z_]\w*` | 变量名的可读标识 |
| 格式 | `Xn` / `XnLE`（n = 偶数 hex 位数 2..16，对应 n/2 字节） | 输出的十六进制位数；`Xn` 大端输出（MSB 在前），`XnLE` 小端输出（LSB 在前，小端协议字段直配免反转公式）。字节序只裁决该占位符自身线序，与 `framing.byteOrder` / `dataByteOrder` 无关 |
| 变长 | `raw` | 变长字节流直插（载荷由写请求运行时注入，仅 `BuildBytes` 入口，见 §9） |

占位符正则：`^\{([A-Za-z_]\w*):(X[0-9]+(LE)?|raw)\}$`（X 后为偶数 2..16；`LE` 后缀 = 该字段按小端线序输出）。

> **`{Frame:fixed}` 语义（长度域计算示例）**：`{Frame:fixed}` 的值 = 模板**定长部分**的总字节数（hex 字面量按字节数、`{Name:Xn}`/`{Name:XnLE}` 按 n/2 字节计；`{Name:raw}` 载荷不计入）。写"长度域 = 帧长 − 常数"时注意 `{Frame:fixed}` **已包含长度域占位符自身宽度**，因此常数 = 长度域之前的头部字节数 + 长度域宽度。示例（FINS/TCP，长度域在魔数之后）：魔数 4B + `{Len:X8}` 4B → `Len = {Frame:fixed} - 8`（= 帧总长 − 8 = ICF 起至帧尾的命令部分长度）。三菱 MC 3E（长度域在 7 字节头内）：`ReqDataLen = {Frame:fixed} - 9`。

> **三段文法已移除**：`{名称:函数关键字:格式}` 形式（`auto` / `calc` / 内联校验和 `crc16modbus` / `crc16ccitt` / `crc32` / `lrc` / `xor8`）**不再合法**，校验器报 Error（规则6）并给出迁移指引。校验和改用声明式 `inputs.source=auto` + `strategy=crc` + `params.algo`（取值 `crc16-modbus` / `crc16-ccitt` / `crc32`）；派生长度改用 `outputs` 的 `derivedLength`。

#### 3.2.1 VariableConfig（inputs / outputs 的 value 结构）

协议级 / 操作级 `inputs` 与 `outputs` 的 map value 均为本结构（对应 C++ `VariableConfig`，见 [Config.hpp L201-222](src/Core/include/MyProt/Core/Config.hpp#L201-L222)）：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `source` | string | ✓ | — | `static`（声明型，含 `value` 则注入、无 `value` 则纯 UI 展示）/ `auto`（自动计算）。仅此两种取值，其余直接报错 |
| `value` | uint32 | ✗ | — | 仅 `source=static` 有效；取值 0~4294967295。**有值** → 注入 `ctx.variables` 作默认值；**无值** → 不注入，仅作 UI 提示 |
| `strategy` | string | `auto` 时必填 | `""` | `autoIncrement` / `frameSlice` / `expr` / `crc` / `derivedLength` |
| `params` | object | ✗ | `{}` | 策略参数（`source=auto` 且非 `derivedLength` 有效），由 AutoComputeProvider 消费。如 `autoIncrement` 的 `{"seed": 1}`（对应 C++ `paramsJson`） |
| `expr` | string | `derivedLength` 必填 | `""` | 派生长度表达式，见 §3.2 派生长度段 |
| `label` | string | ✗ | `""` | 显示名（所有 source 通用，纯 UI） |
| `unit` | string | ✗ | `""` | 单位/量纲后缀（纯 UI） |
| `enum` | array | ✗ | — | 候选值集合，UI 渲染为下拉；元素形式见 §3.3（对应 C++ `enumValues`）。未列入的值仍允许手输 |
| `placeholder` | string | ✗ | `""` | 输入框占位提示（纯 UI），如 `"标签字节地址 (寄存器号×2), 引擎按 merged 覆盖"` |

> **C++ 引擎只消费** `source` / `value` / `strategy` / `params` / `expr` 五个字段；`label` / `unit` / `enum` / `placeholder` 为**纯展示元信息**，缺省不构成校验错误（与 §3.3 的 hint 语义一致）。
>
> **JSON 字段 ↔ POCO 字段**唯一不一致项：`params` ↔ `paramsJson`（`enum` ↔ `enumValues`）——二者为序列化层别名，非 §0 约定 1 的破坏（其余字段同名）。

**完整实例**（modbus-tcp.json 协议级 `inputs` 节选）：

```jsonc
"inputs": {
  "UnitID": {
    "source": "static", "value": 1,
    "label": "从站地址", "unit": "",
    "enum": [1,2,3,4,5,6,7,8,9,10]
  },
  "StartByteAddress": {
    "source": "static", "value": 0,
    "label": "起始字节地址", "unit": "字节",
    "placeholder": "标签字节地址 (寄存器号×2), 引擎按 merged 覆盖"
  },
  "TransactionID": {
    "source": "auto", "strategy": "autoIncrement",
    "params": { "seed": 1 },
    "label": "事务ID"
  }
}
```

> **字节序约定**：变量与自动计算值按格式 `Xn` **大端**输出。**校验和字段的输出线序由 `strategy=crc` 的 `params.byteOrder` 决定**（默认 `little`，适配 Modbus RTU），不套用此大端约定。

**变量解析顺序**（无修饰符的 `{Name:Xn}`）：
1. 先查 `TagDefinition.variables[Name]`（uint32，按大端 N 字节；**已含加载期自 `DeviceConfig.variables` 合并的缺省键**——标签显式声明优先，同名键不被覆盖，见 §4）；
2. 未命中再查 `SessionContext.variables[Name]`（字节数组，按大端整数解释，左侧截断或补零至 N 字节）；
3. 仍未命中 = `BuildError`（Fail-Fast，携带 tag/device 上下文）。

> 该顺序统一了旧设计里"三套变量类型"的歧义：标签变量是 uint32，会话变量是字节数组，模板层负责归一化为定长大端字节。

**自动计算与校验和（声明式）**：模板内**不再有函数关键字令牌**，自动计算与校验和一律由协议/操作级 `inputs` 的 `source=auto` 声明驱动（策略表见 §3.2.1），模板只按 `{名称:Xn}` 消费其结果：

- `strategy=autoIncrement`：引擎内原子自增计数器，取值模 `2^(8n)`，大端输出。计数器为全引擎共享（事务 ID 全局唯一即可，无需按设备隔离）。
- `strategy=expr` / `frameSlice`：由表达式或帧切片求值（如携带长度字段的帧头）。
- `strategy=crc`：校验和。计算范围由 `params` 的 `from` / `to` 指定（默认该字段之前的全部字节），线序由 `params.byteOrder` 决定（默认 `little`）。`params.algo` **必填**，取值为 `crc16-modbus` / `crc16-ccitt` / `crc32`（引擎 `ExecCrc` 仅实现这三个）。算法与参数细节见 modules/02 §2.5；子范围校验见 [ROADMAP.md](./ROADMAP.md)。
- 派生长度见下节 `outputs` 的 `derivedLength`。

**Modbus TCP 读保持寄存器完整示例**（MBAP 头 7 字节 + PDU 5 字节 = 12 字节）：

```jsonc
"requestTemplate": [
  "{TransactionID:X4}",        // 2B 事务 ID（inputs 声明 strategy=autoIncrement）
  "00 00",                     // 2B 协议 ID（固定 0）
  "{Length:X4}",               // 2B = 其后字节数（outputs 声明 derivedLength）
  "{UnitID:X2}",               // 1B 单元标识
  "03",                        // 1B 功能码
  "{StartAddress:X4}",         // 2B 起始地址
  "{RegisterCount:X4}"         // 2B 寄存器数量
]
```

**Modbus RTU 读保持寄存器示例**（无长度字段，Silence 成帧；尾部 CRC16 小端；校验和由 `inputs` 的 `strategy=crc` 声明）：

```jsonc
// inputs: { "Crc": { "source": "auto", "strategy": "crc",
//                    "params": { "algo": "crc16-modbus", "byteOrder": "little" } } }
"requestTemplate": [
  "{UnitID:X2}",               // 1B 从站地址
  "03",                        // 1B 功能码
  "{StartAddress:X4}",         // 2B 起始地址
  "{RegisterCount:X4}",        // 2B 寄存器数量
  "{Crc:X4}"                   // 2B CRC16 = 对前 6 字节计算, 小端输出
]
```

**保留形式**：`{Name:raw}`（变长字节注入）**已实装**——现行写路径（见 §9 首条）以定宽占位符渲染标量值（如 `{WriteValue:X4}`）覆盖单寄存器写场景已被 `{Name:raw}` 取代；`raw` 占位符消费运行期注入的 hex 字节流，**不经过任何大端归一化**，原样追加到请求帧。典型场景：Modbus FC16（写多寄存器）、S7 ANY（变长写）、IEC104 ASDU（信息体）等变长写帧。配套：WebApi `POST /api/data/write` body 的 `bytes` 字段（与 `value` 互斥）提供载荷，引擎以 `writeVariable`（默认 `WriteValue`）为键注入运行期 raw 表。详见 §9 写路径条目与 [ADR-0007](./adr/0007-write-path-scope.md) §「`{Name:raw}` 实装」节。校验器对 `raw` 格式已放行。（v1.32：配置级 `variableBytesHex` 字段已移除——其声明值从不进入帧。）

**变长写派生长度**：写多路操作的请求帧头部常含按载荷大小得出的长度/数量字段。配置驱动方式是在协议/操作级 **`outputs`** 段声明 `source: "auto"` + `strategy: "derivedLength"`，引擎在参数层预解析阶段求值并注入（变长写时按「变长载荷字节数」派生）。

`derivedLength` 用 `expr` 表达——**轻量算术表达式**。载荷字节数经 `{name:len}` 语法引用——`{name:len}` 表示模板中某变量的**字节长度**：若该变量在模板中为 `{Name:raw}` 占位符（变长写载荷，由模板自动识别），取**实际载荷字节数**；否则取其**模板渲染宽度**（`X2`=1、`X4`=2、`X8`=4、`X16`=8 字节）。`expr` 中普通名引用仍查 `inputs` 配置值。无 `payload`/`count` 硬编码保留名，也无需 `payloadLength` 特殊策略声明：

| 引用形式 | 值 | 典型用途 |
|------|-----|---------|
| `{载荷名:len}`（载荷名 = 模板 `{Name:raw}` 占位符名，如 `WriteValue`） | 实际载荷字节数 | Modbus ByteCount、S7 DataLength |
| `{其它模板变量名:len}` | 模板渲染宽度 | 按定宽字段换算（如 `{RegisterCount:len}`） |
| `inputs` 中已声明名（普通引用） | 配置值 | 自定义换算（如 `UnitID + 1`） |

`expr` 支持 `+ - * / % & | ^ ~ ( )` 等算术，如线圈位长度 `"{WriteValue:len} * 8"`、寄存器数 `"{WriteValue:len} / 2"`。`expr` **必填**（校验规则10）；引用域 = 同一作用域 `inputs` 已声明名 ∪ 模板 `{Name:raw}` 载荷占位符名（op 输出可引用 op.inputs ∪ protocol.inputs；`{name:len}` 与普通名均限此域，引用域校验见规则10）。寄存器数在 `expr` 中直接写 `/ 2`。

**模板结构原语（ADR-0012 §1.1）**——表达式可直接引用模板自身的结构量，消除与 `requestTemplate` 的手工常量耦合：

| 原语 | 语义 | 典型用途 |
|---|---|---|
| `{Frame:fixed}` | 当前操作模板**全部非 raw 元素渲染宽度之和**（含长度字段自身宽度；hex 字面量按字节、`{N:Xn}` 按格式宽度、`{N:raw}` 记 0） | 帧总长字段：`"{Frame:fixed} + {WriteValue:len}"`（includesHeader=true） |
| `{Name:offset}` | 占位符 `Name` **首次出现**前累计的字节偏移 | 段长度推导、调试诊断 |

约束：`Frame` 为表达式保留名，模板占位符不得命名 `Frame`（校验 Error）；`{Name:offset}` 引用的名称须为本协议任一操作模板中存在的占位符（校验 Error）。**保存期试算校验**（ADR-0012 §2）：含 `{Name:raw}` 的写操作在保存时以 dummy 载荷真实渲染一帧并核对成帧长度槽位——模板固定段与派生长度/framing 配置不一致、或 `expr` 求值失败，均拒绝保存（400）。

`derivedLength` 仅作用于变长写（含 `{Name:raw}` 载荷的模板）；标签若显式提供同名变量则保持优先、不被覆盖。模板中相应占位符写成普通定宽（如 `{PDULength:X4}`，无 `auto` 段），由注入值渲染。配置示例（modbus-tcp.json `WriteMultipleRegisters` 操作级）：

```jsonc
"operations": {
  "WriteMultipleRegisters": {
    "kind": "write",
    "requestTemplate": [ "...", "{RegisterCount:X4}", "{ByteCount:X2}", "{WriteValue:raw}" ],
    "inputs": {
      "StartAddress":  { "source": "static", "label": "起始寄存器地址" },
      "WriteValue":       { "source": "static", "label": "写入载荷" }
    },
    "outputs": {
      "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} / 2" },
      "ByteCount":     { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}" },
      "PDULength":     { "source": "auto", "strategy": "derivedLength", "expr": "{Frame:fixed} - 6 + {WriteValue:len}" }
    }
  }
}
```

> 上例 `- 6` 为 MBAP 头长（`includesHeader=false` 时长度槽位值 = 帧总长 − headerLength）；
> S7 等含头协议（`includesHeader=true`）直接写 `"{Frame:fixed} + {WriteValue:len}"`。
> 常量形式（如 `{WriteValue:len} + 7`）亦合法（偏移自检 + ADR-0012 试算校验兜底），但推荐用 `{Frame:fixed}` 以消除模板改动时的手工同步。

> **载荷名声明位置**：`WriteValue` 等载荷名声明在**使用它的写操作**的 `inputs` 段（`source: "static"` 无 `value`，仅作 UI 展示），**不放在协议级 `inputs`**——它只在写多路操作里被 `{Name:raw}` 消费，不是全局输入。协议级 `inputs` 只保留跨操作共享的输入（如 `UnitID`/`TransactionID`）。

**数量/长度字段一律由载荷计算**：写多路操作的 `RegisterCount`、`ByteCount`、`PDULength` 全部走 `derivedLength` **自动计算**逻辑——用户在数据点选择写操作类型（线圈 FC 0F / 寄存器 FC 10）后填写入数据，引擎按载荷字节数派生各字段，**无需手填数量**。典型换算（modbus-tcp.json `WriteMultiple*`，载荷变量名 `WriteValue`）：

| 字段 | 操作类型 | `expr` | 语义 |
|---|---|---|---|
| `RegisterCount` | 线圈 FC 0F | `{WriteValue:len} * 8` | 字节 → 线圈数（1 字节含 8 线圈） |
| `RegisterCount` | 寄存器 FC 10 | `{WriteValue:len} / 2` | 字节 → 寄存器数（每寄存器 2 字节） |
| `ByteCount` | 两种 | `{WriteValue:len}` | 载荷字节数 |
| `PDULength` | 两种 | `{WriteValue:len} + 7` | PDU 长度（含 UnitID 起算） |

**载荷变量由模板 `{Name:raw}` 占位符自动识别**：`{Name:raw}` 占位符消费的**变长载荷**字节流由**写请求运行时注入**（`POST /api/data/write` 的 `bytes`，或按 `finalType` 编码的 `value`），键 = `writeVariable`（默认 `WriteValue`），经 `{Name:raw}` 原样拼入帧；其**字节数**由引擎在派生长度求值前按模板 raw 占位符名注入（如 modbus-tcp.json 与 s7-1200.json 的 `"WriteValue"`），`expr` 以 `{载荷名:len}` 引用。**无需任何特殊策略声明**——载荷名在使用它的**写操作** `inputs` 段声明（`source: "static"` 无 `value`，仅作 UI 展示，可省略）；标签显式提供同名标量变量值则优先（`InjectDerivedLengthVariables` 显式优先）。

**线圈整字节假设**：线圈按**位**打包（8 线圈 = 1 字节），仅凭打包字节无法精确反推非整字节数量——`{WriteValue:len} * 8` 隐含"每字节的 8 个线圈全部写入"假设；当实际线圈数不足一个整字节时（如只写 3 个线圈，仍占 1 字节），数量会被估成整字节上限（8）。

**长度偏移自检**：校验器在操作校验时，对**恰好位于成帧长度槽位**（`framing.lengthFieldOffset` / `lengthFieldLength`）且与载荷呈**常量偏移**关系的派生变量（`expr` 形如 `{载荷名:len} ± C`，如 `{WriteValue:len} + 7`），会自动核对声明的偏移与模板固定字节布局是否一致：

```
期望偏移 = [includesHeader=true] 载荷起点字节位 + 尾部固定字节
          [includesHeader=false] 载荷起点字节位 − (长度字段位置 + 宽度) + 尾部固定字节
```

一致则通过；不一致报错（防止协议作者增删模板固定字节后漏改长度偏移而静默错帧）。注意该自检**仅**作用于成帧长度字段槽位的派生变量；`{WriteValue:len} / 2`、`{WriteValue:len} * 8` 等单元换算、非槽位的派生变量（如 S7 `DataLen`）不适用、不误报。

**寄存器数量变量名**（固定）：批量读取合并时注入的「寄存器数量」变量名恒为 `"RegisterCount"`（读路径固有名，无需配置）。写路径的寄存器数量名取自写操作 `outputs` 段 `derivedLength` 派生（如 `RegisterCount`），分别在各自操作域内定义——二者同为 `RegisterCount` 时即"跨操作同名不同角色"（读=输入注入、写=派生输出）。S7 等无寄存器语义的协议模板若未引用则该键不渲染、不产生多余键。

**兜底说明**：PDULength 一律通过 `derivedLength` 声明求值；模板若使用 `{PDULength}` 而未声明为 `derivedLength`，则不注入（保留字面量原值）。

### 3.3 占位符提示元数据（UI 辅助层）

协议作者可为 `requestTemplate` 中出现的**变量型占位符**（`{Name}` / `{Name:Xn}`）附加提示元数据，让标签表单在用户填值时显示可读标签与单位：

```jsonc
"operations": {
  "ReadHoldingRegisters": {
    "requestTemplate": [...],
    "placeholderHints": {
      "StartAddress":  { "label": "起始寄存器地址", "unit": ""    },
      "RegisterCount": { "label": "读取数量",       "unit": "个"  },
      "UnitID":        { "label": "从站地址",       "unit": ""    }
    }
  }
}
```

字段定义：

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `<Name>` | object | — | — | 占位符 Name 作 key；未声明的 Name 视为无提示 |
| ↳ `label` | string | ✗ | `""` | 中文/可读名称（UI 主显示） |
| ↳ `unit`  | string | ✗ | `""` | 单位/量纲后缀（UI 次显示，拼接为 "label / unit"） |
| ↳ `enum`  | array | ✗ | — | 候选值集合。数组 → UI 将 el-input-number 切换为 el-select；**未列入**的值仍允许手输（受控由协议作者决定是否补 `strict`） |

**约束与边界**：

> 展示元信息统一由 `inputs` 声明承载 —— 任一 source 均可携带 `label`/`unit`/`enum`/`placeholder`；纯展示性元信息用 `source: "static"` 且**不写 `value`**（不进 ctx.variables）。统一写法见 §3.2.1（协议级与操作级 `inputs`）。
- 仅作**前端 UI 辅助**——**C++ 引擎不读取展示元数据**，纯展示；缺省/不存在均不构成校验错误。
- 元信息的 key 集合 ⊆ `requestTemplate` 中出现的变量型 Name 集合（提示信息不写也允许，UI 回退到仅显示 Name）。
- 函数型占位符（`crc*` / `lrc` / `xor8`）与 `derivedLength` 派生长度不参与（它们不由用户填值）。
- 与变量值解耦：`label` / `unit` / `enum` 只决定 UI 呈现；变量默认值由 `source=static` 的 `value` 提供，无 `value` 的 `static` 不进 ctx.variables。

**`enum` 元素形式**（UI 候选）：

```jsonc
"enum": [
  1, 2, 3, 4, 5, 6, 7, 8, 9, 10,                // 纯数字字面量 → 直接作候选值
  { "value": 3,  "label": "03 - 读保持寄存器" }, // 对象 → 显示 label，存 value
  { "value": 16, "label": "16 - 写多个寄存器" }
]
```

混合使用合法。`enum` 数组为空 / 缺省 → 保持 el-input-number 形态（不切换）。

**完整示例**（Modbus 的 UnitID 与 S7 的区域码可枚举）：

```jsonc
"inputs": {
  "UnitID": {
    "source": "static", "value": 1,
    "label": "从站地址",
    "enum": [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]          // 裸数字简写: 无标签语义
  },
  "FunctionCode": {
    "source": "static", "value": 3,
    "label": "功能码",
    "enum": [
      { "value": 1,  "label": "01 - 读线圈" },
      { "value": 3,  "label": "03 - 读保持寄存器" },
      { "value": 16, "label": "16 - 写多个寄存器" }
    ]
  },
  "Area": {                                            // S7: 区域码 (替换魔数 + 文案)
    "source": "static", "value": 132,
    "label": "区域码",
    "enum": [
      { "value": 129, "label": "I  输入 (PE)" },
      { "value": 130, "label": "Q  输出 (PA)" },
      { "value": 131, "label": "M  标志位" },
      { "value": 132, "label": "DB 数据块" }
    ]
  }
}
```

**`enum` 额外约束**（v1.32 起由校验器在加载期强制 —— 原先是"前端忽略 + 告警不阻断"）：

- 元素仅支持 `number` 或 `{ "value": number, "label": string }`；其他类型 → **加载期报错**。
- `value` 须为整数且 ∈ [0, 4294967295]；`label` 须为字符串（缺省 = 十进制字面量）。UI 输入侧接受 `0x` 十六进制，JSON 中统一写十进制。
- 重复 `value` → **加载期报错**（候选集不含重复项）。
- 值仍属**展示性候选，不做白名单**：候选值之外用户可照常手输（保留自由度）。如需严格限定，协议作者用 `source=static` 默认值 + 协议层范围校验兜底。

### 3.4 变量就近覆盖与继承

`inputs` / `outputs` 声明可在协议级与操作级出现，同名键遵循**就近覆盖**（操作级 > 协议级），按整条 `VariableConfig` 替换；标签级的 `variables`（标量值表）在运行时进一步覆盖。合并链统一实装于 `RequestBuilder::MergeVariables`。

**三级作用域与优先级**：

| 层级 | 位置 | 粒度 | 说明 |
|------|------|------|------|
| 协议级 | `ProtocolConfig.inputs` | 全协议默认 | `source=static` 提供默认值（合并链基准） |
| 操作级 | `OperationConfig.inputs` | 仅该操作 | 同名键覆盖协议级整条声明 |
| 标签级 | `TagDefinition.variables` | 运行期标量值 | 最高优先，覆盖前两级同名键 |

**典型场景**（如 Modbus `UnitID` 协议级默认 1，某标签覆盖为 3；某操作内仅声明 `RegisterCount` 的 UI 提示）：

```jsonc
// 协议级
"inputs": { "UnitID": { "source": "static", "value": 1 } }
// 操作级（覆盖协议级同名，或新增）— 无 value 的 static 即 UI 提示语义
"ReadHoldingRegisters": { "inputs": { "RegisterCount": { "source": "static", "label": "读取数量" } } }
```

**派生长度的就近语义**：`derivedLength` 注入同样遵循操作级覆盖协议级；标签若显式提供同名变量则优先（不被派生注入覆盖）。

**各 source 的分派去向**（详见 §3.2）：

- `static` 含 `value` — 合并进 ctx.variables，作默认值。
- `static` 无 `value` — 不进 ctx.variables，仅供前端 UI 展示。
- `auto`（`autoIncrement`/`frameSlice`/`expr`/`crc`）— 拼接为 autoComputeJson 喂 `AutoComputeProvider`（渲染期求值）。
- `auto`（`derivedLength`）— 不进 ctx.variables，由 `TagReader.WriteBytes` 按载荷派生注入。

---

## 4. 设备配置 (DeviceConfig)

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `id` | string | ✓ | — | 全局唯一 |
| `protocol` | string | ✓ | — | 引用 `protocolName` |
| `connection` | object | ✓ | — | 见下；其中 `timeoutMs` 仅为**连接建立**超时 |
| `requestTimeoutMs` | int | ✗ | 3000 | **单次请求-应答**超时（一发一收一解析）；该设备所有操作共用，为唯一配置点（2026-08-24 收敛，见 [ADR-0004](./adr/0004-timeout-retry-budget.md) R1） |
| `username` | string | ✗ | null | 协议级认证凭据（如 S7 CPU 密码） |
| `password` | string | ✗ | null | 同上；日志中一律脱敏为 `***` |
| `resilience` | object | ✗ | null | 逐设备韧性策略覆盖；null = 用全局 §11 `resilience`。字段与全局块同构（部分字段可省略，省略项回退全局值） |
| `variables` | map&lt;string, uint32&gt; | ✗ | `{}` | **设备级模板变量缺省**。加载期合并入该设备各标签的有效变量集（同名键以标签声明优先，见 §3.2），如 `UnitID` 等设备常量只声明一次 |
| `variableBytesHex` | map&lt;string, string&gt; | ✗ | `{}` | **已移除**（v1.32）：设备级变长模板变量缺省。运行时核实其声明值从不进入帧（载荷恒由写请求注入，键 = `writeVariable`），构成"声明了也不生效"的陷阱，字段已删除 |

`connection` 采用扁平结构，字段按协议级 `transport` 类型取用：

| 字段 | 类型 | 默认 | 适用 | 说明 |
|------|------|------|------|------|
| `host` | string | — | Tcp/Tls | 主机名或 IP |
| `port` | uint16 | 0 | Tcp/Tls | 0 = 使用协议 `defaultPort` |
| `portName` | string | — | Serial | 覆盖协议级 `portName` |
| `interface` | string | — | CAN | 总线接口名（如 `can0` / `vcan0`）；CAN 当前未实装，见 [ROADMAP.md](./ROADMAP.md) |
| `canId` | uint32 | 0 | CAN | 该节点的 CAN ID（11/29 位） |
| `timeoutMs` | int | 3000 | 全部 | **连接建立**超时（握手步骤超时另见 §2.5） |

**共享总线通道复用**（见 [ADR-0002](./adr/0002-transport-abstraction.md) §4）：`ChannelManager` 以**物理端点**为通道缓存 key（Tcp/Tls = `host:port`、Serial = `portName`、CAN = `interface`）。同一物理端点上的多个逻辑设备（RS-485 从站、CAN 节点）共享同一 `IChannel`，串行化粒度为物理总线；逻辑设备的路由（UnitID / CAN ID）由模板变量承载。

凭据如何生效：`username` / `password` 在握手阶段作为**模板变量**注入（与 `variables` 同等待遇），在 `handshake[].requestTemplate` 中以 `{password:X...}` 等形式消费。框架不做任何隐式认证动作——认证仍是协议行为，框架只负责递送。

```jsonc
// TCP 设备
{
  "id": "PLC-001",
  "protocol": "modbus-tcp",
  "requestTimeoutMs": 3000,
  "connection": { "host": "192.168.1.100", "port": 502, "timeoutMs": 3000 }
}
// 串口设备
{
  "id": "Meter-07",
  "protocol": "modbus-rtu",
  "connection": { "portName": "COM3", "timeoutMs": 1500 }
}
```

---

## 5. 标签配置 (TagDefinition)

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `name` | string | ✓ | — | 全局唯一，建议 `"<deviceId>.<测点名>"` 风格 |
| `deviceId` | string | ✓ | — | 引用 `DeviceConfig.id` |
| `operation` | string | ✓ | — | 引用该设备协议中的操作名（读语义；`direction=write` 写标签时即写操作） |
| `variables` | map\<string, uint32\> | ✗ | `{}` | 模板变量，统一用**跨协议字节单位**，如 `{"StartByteAddress": 0, "ByteCount": 4}`；协议族单位（`StartAddress` / `RegisterCount`）由协议 JSON 的 `outputs` 派生，不写在标签里 |
| `variableBytesHex` | map\<string, string\> | ✗ | `{}` | **已移除**（v1.32）：标签级变长模板变量。载荷由写请求运行时注入（键 = `writeVariable`），声明值从不进入帧；固定字节块请用模板 hex 字面量 |
| `writeOperation` | string | ✗ | `""` | **标签级写能力（标量写）**：`POST /api/data/write` 携带 `value` 时使用的操作名（须为所引协议的写类操作）。空 = 不可标量写；非空 = 读写标签，读回校验用自身 `operation` |
| `writeBytesOperation` | string | ✗ | `""` | **标签级写能力（变长写）**：`POST /api/data/write` 携带 `bytes` 时使用的操作名（须为所引协议的写类操作，模板以 `{Name:raw}` 消费载荷）。空 = 不可变长写；仅声明 `writeOperation` 的标签，变长写回退复用其 `writeOperation` |
| `writeVariables` | map\<string, uint32\> | ✗ | `{}` | 写请求专用变量覆盖，在 `variables` 之上合并（同名键覆盖），如 S7 写的 `TransportSize`/`Length` 与读不同 |
| `scanRateMs` | int | ✗ | 1000 | 轮询周期，必须 > 0 |
| `finalType` | string | ✗ | `"UInt16"` | 见 §6 |
| `byteOrder` | ByteOrder | ✗ | null | 数据字节序覆盖；null 时按回退链取值 |
| `bitOffset` | int | ✗ | -1 | **位偏移**（0–7，语义 = `StartByteAddress` 所指字节内的位偏移）；`-1` = 未声明，越界为 Error（规则13）。仅 Bool 取位使用：声明后按 `(raw[0] >> bitOffset) & 1` 取位；未声明按整字节 `raw[0] != 0`。协议 `outputs` 的位寻址 `expr` 通过运行时注入的 `BitOffset` 引用同一值 |
| `coalesce` | bool | ✗ | `true` | 是否参与地址邻近合并；单地址读（如 S7 `ReadVar*`）设 `false` |
| `direction` | string | ✗ | `"read"` | `read`（参与轮询，写能力由 `writeOperation` / `writeBytesOperation` 声明）/ `write`（**只写标签**，`operation` 即写操作，不参与轮询，见下） |
| `writeVariable` | string | ✗ | `"WriteValue"` | 写值注入的模板变量名，模板以 `{writeVariable:X4}`（标量）或 `{writeVariable:raw}`（字节/浮点）消费 |
| `readBackTag` | string | ✗ | `""` | 写标签：写后读回校验引用的**读标签名**；空时回退为按写目标自身读语义重读（仅写标签可配） |
| `converters` | array | ✗ | `[]` | **读后换算链**（C1）：采集值→工程值，按序应用。v1 仅 `scale` 项 `{"kind":"scale","k":<num>,"b":<num>}`（`value' = value×k + b`，先乘后加），换算后值统一为 Double。仅数值型 `finalType`（整型/Float/Double）的**读标签**可配；写标签或非数值类型声明即校验错误（规则13）。典型：0.1°C/bit → `k=0.1,b=0`；4-20mA 标定 → `k=(量程上限-下限)/27648, b=下限` |

**字段命名裁决**：旧 JSON 示例中 `name` 与 POCO 的 `tagName` 统一为 **`name`**（POCO 字段随之改名）；`pollIntervalMs` 统一为 **`scanRateMs`**（保持 SCADA 术语，与架构文档的 scanRate 分组一致）。运行时输出对象 `TagValue.tagName` 保持不变（它是携带设备上下文的值对象，字段语义无歧义）。

**标签级写能力（writeOperation / writeBytesOperation，唯一写声明点）**：
- 写能力**只在标签层声明**（协议级 `writeOperation` / `writeBytesOperation` 兜底已移除）——只读标签的语义就是「写 API 必须拒绝」，不再因协议声明而隐式可写。
- `writeOperation` 非空 = 可**标量写**（`POST value`）；`writeBytesOperation` 非空 = 可**变长写**（`POST bytes`）；两者可同时声明（全声明的读写标签）。写请求直接定向本标签，照常轮询 `operation`。
- 写请求变量表合并链：协议级/操作级 `inputs`（`source=static`）→ `tag.variables` → `tag.writeVariables`（同名键覆盖）→ 运行时注入 `{writeVariable}` 与派生量。
- 标量按 `finalType` 编码：`Float`/`Double` → IEEE754 大端字节走 `{writeVariable:raw}`；整数族 → `{writeVariable:X?}`。
- `readBack=true` 时用**自身** `operation` 的读语义重读（`readBackTag` 无需配置），以读 op `dataStartIndex` 为数据区起点逐字节比较。
- 操作语义标注：协议操作可选 `kind: "read" | "write"`；校验器核对标签 `operation`（读标签须 read 类 / 写标签须 write 类）与 `writeOperation` / `writeBytesOperation`（须 write 类）的一致性，UI 表单按 kind 过滤下拉。

**只写标签（direction=write）**：
- 覆盖**无读语义**的罕见场景：`operation` 直接指向写操作模板（如 s7-1200 的 `WriteVarBit`/`WriteVarWord`、modbus 的 `WriteSingleRegister`），写语义（TransportSize/Length/DBNumber/AddrLo/…）全部由 `variables` 决定；`scanRateMs` 不作要求，不参与轮询。
- `POST /api/data/write` 以 `tag` 字段定向写标签名；标量按 `finalType` 编码（`Float`/`Double` → IEEE754 大端字节走 `{writeVariable:raw}`，整数族 → `{writeVariable:X?}`）。
- `readBack=true` 时按 `readBackTag`（缺省写目标自身）对应读标签的读 op 重读，以读 op `dataStartIndex` 为数据区起点逐字节比较。
- 运行时自动注入变量：`StartByteAddress` / `ByteCount`（跨协议字节单位，**仅此二者**为引擎契约）+ 标签 `bitOffset` 注入的 `BitOffset`。其余派生名（`StartAddress` / `RegisterCount` / `PDULength` / `DataLen` …）一律由协议 JSON 的 `outputs(derivedLength)` 自行声明；规则14 的解析域白名单**从 `protocol.outputs` 与 `op.outputs` 动态收集**，不硬插具体名。

**byteOrder 回退链**：`tag.byteOrder` → `protocol.dataByteOrder` → `BigEndian`（三者均缺省时）。

**上报过滤（已移除）**：此前存在的 `deadband` / `reportMode` 两个标签字段**已删除**——它们无任何运行时消费方（唯一结果出口 `onResults` 直连 `LatestValueStore`，在该处过滤会让"最新值缓存"失真），保留字段等于承诺了不存在的行为。上报过滤作为独立扩展登记在 [ROADMAP.md](./ROADMAP.md)，需先有发布层设计。

```jsonc
{
  "name": "PLC-001.Temperature",
  "deviceId": "PLC-001",
  "operation": "ReadHoldingRegisters",
  "variables": { "StartByteAddress": 4, "ByteCount": 4 },
  "scanRateMs": 5000,
  "finalType": "Float"
}
```

> 地址跨度由协议 JSON 通过 `outputs.ByteCount` 的 `derivedLength` 表达（Modbus: `variables.RegisterCount × 2`；S7: 直接字节数），引擎不持有"寄存器 = 2 字节"的协议族假设。位偏移为标签一等字段 `bitOffset`（见上表）。

---

## 6. 类型系统

**finalType 允许值及最小字节数要求**（原始数据不足时返回 `TypeConversionError`）：

| finalType | 字节数要求 | 转换结果 (TypedValue 备份) |
|-----------|:--:|------|
| `ByteArray` | ≥ 0 | `vector<uint8_t>`（原样） |
| `UInt16` / `Int16` | ≥ 2 | uint16 / int16 |
| `UInt32` / `Int32` | ≥ 4 | uint32 / int32 |
| `UInt64` / `Int64` | ≥ 8 | uint64 / int64 |
| `Float` | ≥ 4 | float（IEEE 754，bit_cast） |
| `Double` | ≥ 8 | double |
| `Bool` | ≥ 1 | bool（首字节非零为 true） |
| `String` | ≥ 0 | string（原字节序列） |

**ByteOrder 枚举**：`BigEndian` / `LittleEndian` / `WordBigByteLittle`（字内大端、字间小端，Melsec 风格） / `WordLittleByteBig`。混合字节序仅对 ≥ 4 字节的多字数据有定义；对 1~3 字节数据使用混合序按 `BigEndian` 处理并在加载期给予 Warning。

---

## 7. 校验规则清单 (ConfigValidator)

Validate 收集**全部**错误后一次性返回（不提前终止），Warning 不阻止启动。

> **前置门禁（版本，先于下列字段校验）**：解析 JSON 后先校验 `schemaVersion`（[ADR-0005](./adr/0005-config-versioning.md)）。缺省 = Warning + 假定当前代际；存在但 `≠ kSupportedSchemaVersion` = 立即返回 `ConfigError`（过老/过新分别给出迁移/升级提示），**不再继续字段校验**；协议文件 `schemaVersion` 与配置根不一致 = `ConfigError`。版本门禁是本清单中**唯一会提前终止**的检查。

**协议层**
1. `protocolName` 非空且全局唯一。
2. `transport.type` / `framing.type` 为合法判别值；各类型必填字段齐备（如 Tls 的 caFile 路径存在性仅 Warning）。
3. LengthField：`lengthFieldLength ∈ {1,2,4}`；`lengthFieldOffset ≥ 0`；`headerLength ≥ lengthFieldOffset + lengthFieldLength`（或为 0）；`maxFrameSize > headerLength`。
4. Fixed：`fixedLength > 0`。
5. Silence：`charTimeUs ≥ 0`、`frameGapUs ≥ 0`、`maxFrameSize > 0`；`charTimeUs == 0` 时协议级 `baudRate` 必须 > 0（否则无法折算字符时间）。`framing.type == "Message"` 为**名称预留**（CAN 未实装，见 [ROADMAP.md](./ROADMAP.md)）。
6. `operations` 非空；每个操作的 `requestTemplate` 非空且逐行匹配 §3 文法；占位符格式合法。
7. 模板函数 token 为内置能力，按内置白名单校验合法性。校验和占位符的格式宽度须与算法匹配（`crc16*`→X4、`crc32`→X8、`lrc`/`xor8`→X2），不匹配报 Error。
8. handshake 的 `sessionExtractExpr` 与 `sessionVariable` 成对出现。
9. 协议层不含仿真字段；仿真校验见 [§13](#13-服务端配置-serverconfig) `ServerConfig.simulation`。
10. `variables` 中 `source=static` 的 `value`（若提供）须为整数（0~4294967295）；`source` 仅 `static` / `auto`（其余取值报错）；同名键优先级 = 标签层 `variables` > 操作层 `variables` > 协议层 `variables(static)`（合并实装 `RequestBuilder::MergeVariables`）；`source=auto` 须给合法 `strategy`，`derivedLength` 须声明 `expr`（引用 `inputs` 已声明名的算术表达式，`{name:len}` 取字节长度；规则10、长度偏移自检见 §3.2）。

**设备层**
10. `id` 非空且全局唯一；`protocol` 引用存在的协议。
11. Tcp/Tls 设备 `connection.host` 非空；`port == 0` 时协议 `defaultPort` 必须 ≠ 0。
12. Serial 设备 `connection.portName` 或协议级 `portName` 至少一个非空。

**标签层**
13. `name` 全局唯一；`deviceId` / `operation` / `writeOperation` / `writeBytesOperation` 引用存在；`direction ∈ {read, write}`；`readBackTag` 仅写标签（`direction=write`）可配，且须引用存在的**读标签**；`writeOperation` / `writeBytesOperation` 须引用协议中 **write 类**操作；操作 `kind` 标注时核对语义一致性（读标签 `operation` 须 read 类、写标签 `operation` 须 write 类）。
14. `scanRateMs > 0`（写标签豁免，不参与轮询）、`finalType` 在 §6 表内、`byteOrder` 在枚举内；写标签 `writeVariable` 非空。
15. 模板占位符必须能在 `tag.variables` 中找到同名项，或属于握手 `sessionVariable` 声明集；否则 Error（把运行时 BuildError 提前到启动期）。自动计算由 `inputs` 的 `source=auto` 声明驱动（模板内不写 `:auto:`/`:calc:` 令牌，三段文法亦已移除）。写路径模板的解析域额外放行 `writeVariable` 与运行时注入量（`PDULength`/`DataLength`/`DataBits`/`DataLen`/`RegisterCount`/`ByteCount`/`StartAddress`）；`writeOperation` / `writeBytesOperation` 模板的解析域为 `variables ∪ writeVariables`。

---

## 8. 字段命名与取值约定

**十六进制与占位符**

- 十六进制字面量统一为**空格分隔字节序列**（如 `03 00 00 06`）；不接受 `"0x03"` 单字节写法。
- 变长载荷由模板 `{Name:raw}` 占位符自动识别，`expr` 用 `{载荷名:len}` 引用实际字节数；载荷名声明在使用它的写操作 `inputs` 段（`source=static` 无 `value`，仅作 UI 展示，可省略）。

**字段命名**

| 位置 | 约定 |
|------|------|
| framing 长度字段宽度 | `lengthFieldLength`（与 POCO / Netty 术语一致） |
| 标签名 | `name`（不是 `tagName`） |
| 轮询周期 | `scanRateMs`（不是 `pollIntervalMs`） |
| 设备连接 | `connection{host, port, timeoutMs}` 嵌套表达；`timeoutMs` 为**连接建立**超时 |

**超时归属**

- 连接建立超时 = `device.connection.timeoutMs`；单次请求-应答超时 = `device.requestTimeoutMs`（[ADR-0004](./adr/0004-timeout-retry-budget.md) R1）。协议与操作级**不设** `timeoutMs`。

**操作级不设的字段**

- `functionCode`：功能码已在 `requestTemplate` 中作为字面量，重复维护必然漂移。
- `retryCount`：重试由韧性策略 + `IsRetryable` 统一裁决（写操作天然不重试），见 §11。
- `expectedResponseLength`：帧长判定完全由 framing 解析器承担。
- `dataType` / `responseParser.valueType`：解析器只出原始字节，最终类型由 `tag.finalType` 决定，避免双源。
- `responseTemplate`（解析侧）：解析不需要应答模板；仿真侧的自定义应答能力挂在 `simulation.operations[].responseTemplate`（[modules/10 §10.6](./modules/10_Simulation.md)）。
- `validCondition`：归属 `responseParser.validCondition`。
- `converters`（位提取等后处理链）：见 [ROADMAP.md](./ROADMAP.md)。

**握手凭据**

- `DeviceConfig.username` / `password` 作为握手模板 `{Username}` / `{Password}` 的取值来源，不作独立传输层字段。

> **配置代际**（[ADR-0005](./adr/0005-config-versioning.md)）：当前 `kSupportedSchemaVersion = 2`，即本文档 §2 字段集（`inputs`/`outputs` 分组 + `derivedLength` 派生长度）。破坏性格式变更使代际递增，并提供自动迁移工具。

---

## 9. 扩展项索引

尚未实装的扩展项统一登记在 [ROADMAP.md](./ROADMAP.md)（含写后 read-back、CAN / TLS / RBAC / 协程化 / 滚动日志 / alertSink / converters / `{Crc:*}` 子范围校验 / 端点细分限流等）。每项的状态、依赖与位置见该表。

---

## 10. 文档维护指引

主文档（`Config_Schema.md` / `architecture/*.md` / `modules/*.md`）描述**当前 v1.0 实态**，不记录变更史；尚未实装的扩展项统一登记在 [ROADMAP.md](./ROADMAP.md)。

模块文档若与本 schema 冲突，**以本文档为准**（schema 是配置契约的唯一事实来源）。

---

## 11. 韧性策略 (resilience)

> **裁决来源**: [ADR-0004](./adr/0004-timeout-retry-budget.md)（超时与重试的时间预算模型）。本节是 ADR-0004 参数在配置契约中的落地点；语义、公式与推导以 ADR-0004 为准。

`resilience` 是 `ConfigRoot` 顶层**可选**块（缺省取全表默认），并允许 `DeviceConfig.resilience?`（见 §4）逐设备覆盖。覆盖采用**字段级回退**：设备块中省略的字段回退到全局值，全局再省略则取本表默认。

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `maxAttempts` | int | 3 | 读操作总尝试次数（含首次）；写操作恒为 1（`IsRetryable` 语义，配置值对写无效） |
| `backoffBaseMs` | int | 100 | 指数退避基准 |
| `backoffMaxMs` | int | 1000 | 单次退避上限 |
| `failureThreshold` | int | 5 | 连续**逻辑读取失败**（预算耗尽）次数 → 熔断打开 |
| `cooldownMs` | int | 10000 | 熔断打开持续时长；期间请求直接 `CircuitOpen` 快速失败，不发起 I/O |
| `halfOpenProbes` | int | 1 | 半开态探测次数；成功 → 闭合，失败 → 重新打开 |

**退避公式**：第 k 次重试前等待 = `min(backoffMaxMs, backoffBaseMs × 2^(k-1))` + 抖动。

**deadline（总截止时间）来源**：

| 场景 | deadline | 说明 |
|------|----------|------|
| 轮询读取 | 由 PollGroup 的 `scanRateMs` 决定 | 一次读取须在一个扫描周期内完成，超预算标记 Bad 并跳过，**不级联**到下一周期 |
| 按需读取（WebApi 单标签） | `device.requestTimeoutMs × maxAttempts`，上限 10000 | 无扫描周期约束；可由请求显式覆盖 |

**最坏耗时**：`worst_case = min(deadline, maxAttempts × device.requestTimeoutMs + Σ backoff(k))`。轮询场景被 `scanRateMs` 封顶，按需场景被 10000ms 封顶。

**校验**（归 §7 设备级顶层，不另编号）：`maxAttempts ≥ 1`、`backoffBaseMs ≥ 0`、`backoffMaxMs ≥ backoffBaseMs`、`failureThreshold ≥ 1`、`cooldownMs ≥ 0`、`halfOpenProbes ≥ 1`；越界报 Error。`DeviceConfig.resilience` 字段集须为全局块子集（未知字段报名称错误）。

```jsonc
// ConfigRoot 顶层
{
  "resilience": {
    "maxAttempts": 3,
    "backoffBaseMs": 100,
    "backoffMaxMs": 1000,
    "failureThreshold": 5,
    "cooldownMs": 10000,
    "halfOpenProbes": 1
  },
  "devices": [ /* ... */ ],
  "tags": [ /* ... */ ]
}
// 设备级覆盖（慢设备放宽冷却，其余回退全局）
{
  "id": "PLC-Slow",
  "protocol": "s7",
  "connection": { "host": "192.168.1.50", "timeoutMs": 5000 },
  "resilience": { "cooldownMs": 30000 }
}
```

---

## 12. 管理面安全 (webApi)

> **裁决来源**: [ADR-0008](./adr/0008-management-plane-security.md)（管理面 TLS / 鉴权 / 限流 / 绑定）。本节是 ADR-0008 参数在配置契约中的落地点；安全语义以 ADR-0008 为准，端点与宿主实现见 [modules/07_WebApi](./modules/07_WebApi.md)。

`webApi` 是 `ConfigRoot` 顶层**可选**块（缺省取全表默认）。管理面（北向）安全等级不应低于设备侧（南向）。

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `bindAddress` | string | `"127.0.0.1"` | 监听地址；默认仅环回，远程管理需显式改 `0.0.0.0`（强烈建议同时启用 TLS + token） |
| `certFile` | string | — | TLS 证书路径；与 `keyFile` 齐备即启用 `httplib::SSLServer` |
| `keyFile` | string | — | TLS 私钥路径；与 `certFile` 齐备即启用 TLS |
| `requireAuth` | bool | `false` | true 时 bearer token 缺失即启动 Fail-Fast（生产建议 true）；false 可免 token（启动 Warning） |
| `rateLimitRps` | int | 5 | 敏感端点令牌桶每秒速率；当前实现为全局令牌桶（含 `/api/data/write` 写端点在内统一计数），超限返回 `429 Too Many Requests`；按端点细分见 [ROADMAP.md](./ROADMAP.md) |
| `rateLimitBurst` | int | 10 | 令牌桶突发上限；超限返回 `429 Too Many Requests` |

**token 来源**：token 本身**不经配置块明文存放**，优先取环境变量 `MYPROT_API_TOKEN`；日志中一律脱敏。`requireAuth = true` 且环境变量为空 = 启动失败（`ConfigError`）。

**默认行为**：未配证书 = 明文 HTTP，启动日志 Warning；非环回绑定且 `requireAuth=false` 时另打印 WARN（管理面写/配置接口无认证暴露）。管理面未启用 TLS，仅建议用于本机/受信任网络。响应附带安全头 `X-Content-Type-Options: nosniff`、`Cache-Control: no-store`。

**校验**（归 §7 顶层，不另编号）：`bindAddress` 须为合法 IP 地址；`certFile` 与 `keyFile` 必须**同时为空或同时非空**（仅配其一报 Error）；`rateLimitRps ≥ 1`、`rateLimitBurst ≥ rateLimitRps`；越界报 Error。`webApi` 为可选块，缺省即全部默认值，**不触发** `schemaVersion` 代际递增（ADR-0005）。

```jsonc
// ConfigRoot 顶层（生产示例：启用 TLS + 强制鉴权）
{
  "webApi": {
    "bindAddress": "0.0.0.0",
    "certFile": "/etc/myprot/certs/api.crt",
    "keyFile": "/etc/myprot/certs/api.key",
    "requireAuth": true,
    "rateLimitRps": 5,
    "rateLimitBurst": 10
  },
  "devices": [ /* ... */ ],
  "tags": [ /* ... */ ]
}
```

---

## 13. 服务端配置 (ServerConfig)

> 将"服务端行为"（仿真）从 `ProtocolConfig` 抽离，全局单实例。`alertSink` / `webhook` 等未实装字段见 [ROADMAP.md](./ROADMAP.md)。

### 13.1 文件

`<dir>/server.json`（与 `tags.json` 同级）。可缺失；缺失 = 默认关闭仿真（`simulation.listenPort = 0`）。

### 13.2 顶层字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `schemaVersion` | int | ✗ | `kSupportedSchemaVersion` | 同 §0 约定；缺省 = Warning + 假定当前代际 |
| `simulation` | SimulationConfig | ✗ | `{}` (关闭) | 仿真端配置；schema 与 §2.6 同（字段名/类型/校验规则一致） |

### 13.3 SimulationConfig 字段

| 字段 | 类型 | 必填 | 默认 | 说明 |
|------|------|:--:|------|------|
| `listenPort` | uint16 | ✓ | 0 | 仿真服务 TCP 监听端口（仅绑定环回 127.0.0.1）；`0` = 不启用 |
| `registerCount` | int | ✓ | 65536 | 仿真数据 16 位寄存器阵列长度，取值 1~65536 |
| `initialValues` | map&lt;string, uint32&gt; | ✗ | `{}` | 寄存器初值；键 = 地址十进制字符串，值 = 0~65535 |
| `operations` | map&lt;string, SimOperationConfig&gt; | ✗ | `{}` | 操作级仿真行为；key 须为 `protocols/*.json` 中任一协议的 `operations` 已声明的操作名 |

**SimOperationConfig** 字段见 [§2.6](#26-simulationconfigv11-移出)。

### 13.4 校验

加载器解析 `server.json` 后做：

1. 顶层须为对象；否则 `ConfigError`
2. `schemaVersion`（如存在）须等于 `kSupportedSchemaVersion`，否则 `ConfigError`
3. `simulation.listenPort ∈ [0, 65535]`
4. `simulation.registerCount ∈ [1, 65536]`
5. `simulation.initialValues` 值须为整数
6. `simulation.operations[].kind ∈ {"read", "write"}`
7. `simulation.operations[].kind == "read"` 时 `countVar` 必填
8. `simulation.operations[].responseTemplate`（可选数组）逐行匹配仿真模板文法——空行 / hex 字面量 / `{data}` / `{req:N:M}`（数字格式），非法即拒绝加载

### 13.5 最小示例

```jsonc
{
  "schemaVersion": 1,
  "simulation": {
    "listenPort": 11520,            // 仅环回监听; 0 = 不启用
    "registerCount": 65536,
    "initialValues": { "0": 5, "1": 100 },
    "operations": {
      "ReadHoldingRegisters": {
        "kind": "read",
        "addressVar": "StartAddress",
        "countVar": "RegisterCount"
      },
      "PresetSingleRegister": {
        "kind": "write",
        "dataOffset": 6
      }
    }
  }
}
```

### 13.6 与协议关系

- `ServerConfig.simulation` 与 `ProtocolConfig` 解耦，**多协议共享同一仿真端**
- `SimulationServer` 接受 `ProtocolConfig &`（取协议语法知识：operations / framing / transport）与 `SimulationConfig &`（取数据行为：listenPort / registerCount / initialValues /operations）

---

> **相关文档**: [模块索引](./README.md) · [ADR-0001 设备内并发模型](./adr/0001-device-concurrency-vs-throughput.md) · [ADR-0002 传输抽象与多总线](./adr/0002-transport-abstraction.md) · [ADR-0004 超时与重试时间预算](./adr/0004-timeout-retry-budget.md) · [ADR-0008 管理面安全](./adr/0008-management-plane-security.md)

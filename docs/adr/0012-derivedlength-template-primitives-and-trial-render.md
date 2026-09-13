# ADR-0012：派生长度表达式的模板结构原语与保存期试算校验

| 项 | 内容 |
|----|------|
| 状态 | **提议（PROPOSED）** · 2026-09-06 |
| 日期 | 2026-09-06 |
| 相关文档 | [ADR-0007](./0007-write-path-scope.md)（写路径 `{Name:raw}`）· [ADR-0010](./0010-vs2015-cpp11-toolchain.md)（C++11 工具链）· [Config_Schema.md](../Config_Schema.md) §2.4/§3.2（`outputs` / `derivedLength`）· [ADR-0003](./0003-known-issues.md)（销账登记入口） |
| 提案来源 | 配置评审：S7 WriteVar `PDULength = {WriteValue:len} + 35` 的 `+35` 与模板固定段为**手工对应关系**，无机器保证 |

---

## 背景

### 1. 现状：派生长度与模板布局是"手数出来的耦合"

变长写操作（Modbus FC16、S7 WriteVar）的 `outputs.derivedLength` 表达式中，帧长度字段以**手工常量**引用模板固定段宽度：

| 配置 | 表达式 | 常量来源（人工核对） |
|------|--------|----------------------|
| `configs/protocols/s7-1200.json` → WriteVar → PDULength | `{WriteValue:len} + 35` | 模板 19 个固定元素累计 35 字节（2026-09-06 逐元素复核成立） |
| `configs/protocols/modbus-tcp.json` → WriteMultipleRegisters → PDULength | `{WriteValue:len} + 7` | `固定段 13 字节 − MBAP 头 6 字节 = 7` |
| 同上 → ByteCount / RegisterCount / DataBits 等 | `{WriteValue:len}` / `… * 8` / `… / 2` | 语义派生，不依赖布局 |

**风险场景**：用户在 `requestTemplate` 中增删任意固定字节（如加一个填充 `00`），`outputs` 中的常量不会报错，渲染出的帧长度字段即告错误——运行时表现为设备不响应或解析错位，且无任何校验期信号。

### 2. 现有防护（长度偏移自检）及其缺口

`ConfigValidator::ValidateDerivedLengthOffsets`（[ConfigDeepValidator.cpp L1241](../../src/Service/src/ConfigDeepValidator.cpp)）已对**成帧长度槽位**上的派生变量做偏移自检：解析 `"{WriteValue:len} + K"` 形态，比对 K 与模板布局期望值。经核验：

- **已覆盖**：Modbus `+7`、S7 `+35` 这类"`len + 常量`"且变量恰好位于 `framing.lengthField` 槽位的表达式；
- **缺口 A（静默跳过）**：表达式形态无法被 `TryPayloadOffset` 解析（如引用了 `{Name:len}` 以外的写法、多变量组合、变量名拼错）时 `known=false → continue`，**静默放行**——派生长度求值失败后该字段按 0 渲染，同样无信号；
- **缺口 B（仅长度槽位）**：非长度槽位的 outputs（DataLen / ByteCount / RegisterCount）完全不在自检范围内；
- **缺口 C（语法级）**：expr 本身的语法合法性（MiniExpression 能否 Parse）在保存期不校验，拼写错误同样落到运行时静默 0 值。

### 3. 表达式引擎现状

`MiniExpression`（递归下降）支持 `+ - * / %`、位运算 `| ^ & ~`、括号、普通变量引用（查值池）、`{name:len}`（查字节宽度表）。求值入口 `AutoComputeProvider::ResolveDerivedLength`（[AutoComputeProvider.cpp L526](../../src/Engine/src/AutoComputeProvider.cpp)）。**缺的不是算术能力，是"模板结构感知"**——表达式无法引用"模板固定段总宽""占位符帧内偏移"这类由 `requestTemplate` 唯一决定的结构量。

---

## 决策驱动因素

1. **模板是帧结构的唯一事实来源**——任何对布局的二次手工描述（魔数常量）都应可推导或受校验；
2. **错误必须在保存期拦截**（Fail-Fast 原则，与配置加载链路一致），而非运行时静默错帧；
3. **向后兼容**：现有 `len + 常量` 表达式与长度偏移自检继续有效，新原语为**纯加法**；
4. **C++11 / VS2015 约束**（ADR-0010）：无结构化绑定 / 无 `std::variant`，接口走 C++11 风格。

---

## 提案 ①：模板结构原语

### 1.1 新原语语义

| 原语 | 语义 | 取值来源 |
|------|------|----------|
| `{Frame:fixed}` | 当前操作模板中**全部非 raw 元素的渲染宽度之和**（含长度字段自身宽度；hex 字面量按字节、`{N:Xn}` 按格式宽度、`{N:raw}` 记 0） | 模板扫描 |
| `{Name:offset}` | 占位符 `Name` **首次出现**处之前累计的字节偏移（raw 占位符取其前缀偏移） | 模板扫描 |

替换效果示例：

```
S7  PDULength:  "{WriteValue:len} + 35"
             →  "{Frame:fixed} + {WriteValue:len}"          (includesHeader=true, adj=0)

Modbus PDULength: "{WriteValue:len} + 7"
             →  "{Frame:fixed} - 6 + {WriteValue:len}"      (−6 = MBAP 头, 属成帧语义常量;
                                                             可暂不迁移, 长度偏移自检已覆盖)
```

**保留名约定**：`Frame` 为表达式保留名，模板中不得出现名为 `Frame` 的占位符（校验器报错）。

### 1.2 结构与接口变更（C++11）

新增共享结构，置于 Engine 层（Gateway 与 Service 复用同一实现，避免双实现漂移）：

```cpp
// AutoComputeProvider.hpp
struct TemplateLayout {
    std::unordered_map<std::string, uint32_t> widths;   // 占位符名 → 渲染宽度 (raw = 0xFFFFFFFF 标记)
    std::unordered_map<std::string, uint32_t> offsets;  // 占位符名 → 首次出现前累计偏移
    uint32_t fixedTotal;                                // 非 raw 元素宽度合计
};
```

```cpp
// ResolveDerivedLength 增加可选布局参数 (nullptr = 旧行为, 兼容既有调用)
static bool ResolveDerivedLength(
    const std::string& expr,
    const std::unordered_map<std::string, uint32_t>* inputs,
    const std::unordered_map<std::string, uint32_t>* varLen,
    const TemplateLayout* layout,          // ← 新增, 末位前插入或以重载提供
    uint32_t& out);
```

- `TemplateWidthTable`（[TagReader.cpp L140](../../src/Gateway/src/TagReader.cpp)）升级为 `BuildTemplateLayout(requestTemplate, layout&)`，一次扫描同时产出 widths / offsets / fixedTotal；
- `InjectDerivedLengthVariables`（现为 TagReader.cpp 匿名命名空间函数）**上移为 Engine 公开静态方法**，`WriteBytes` 与校验器共用；这是 ② 能复用真实注入管线的前提。

### 1.3 布局扫描规则（与模板宽度表对齐）

| 模板元素 | 宽度 | 对 offsets 的贡献 |
|----------|------|-------------------|
| hex 字面量（空格分隔） | 字符数 / 2 | 先记当前 cursor 为偏移，再累加 |
| `{N:X2}` / `X4` / `X8` / `X16` | 1 / 2 / 4 / 8 | 同上 |
| `{N:raw}` | 0（标记值） | 记偏移，宽度 0 |
| 未知格式 | 0 + 置 `hasUnknown` 标志（校验器报错，布局按 0 计） | 同上 |

---

## 提案 ②：保存期试算校验（trial render）

### 2.1 检查范围

`framing.type == LengthField` 且操作模板含 `{Name:raw}` 占位符的**写操作**（即变长写路径——正是魔数耦合的源头）。读操作与定长写不涉及派生帧长，不检查（避免动态长度字段的误报）。

### 2.2 检查步骤

对每个命中操作，用**真实渲染管线**（与 `TagReader::WriteBytes` 完全一致的变量装配链）试渲染一次：

1. 变量表 = `CollectStaticVariables(protocol)` ∪ `OpStaticVariables(op)` ∪ `{StartAddress: 0}`；
2. 注入派生长度：`InjectDerivedLengthVariables(variables, …, dummyPayloadLen=3, BuildTemplateLayout(op.requestTemplate))`；
3. `AutoIncrementProvider` 声明协议 autoCompute 规则（TransactionID 等自增变量取任意值）；
4. `RequestBuilder::BuildBytes(op, variables, {rawName: "01 02 03"})` 得到渲染帧；
5. **比对**：按 framing 语义计算长度槽位期望值，与渲染帧中该槽位实际字节比对：

```
expected = lengthIncludesHeader ? (total − adjustment)
                                : (total − headerLength − adjustment)
其中 total = 渲染帧实际字节数
```

6. **结论**：
   - 槽位实际值 ≠ expected → **error**（"模板固定段与派生长度/成帧配置不一致"——同时捕获魔数过期与 framing 误配两类错误）；
   - 某派生 expr 求值失败（语法错 / 引用未知变量 / 引用未知原语）→ **error**（封堵静默 0 值路径）；
   - 模板含未知格式占位符 → error（沿用规则 6 语义，此处补布局视角）。

### 2.3 挂接点

| 入口 | 位置 | 说明 |
|------|------|------|
| 保存门禁 | `ConfigStore::Validate` Protocol 分支，深度校验通过后追加 | PUT /api/config/protocols/{name} 即受保护 |
| 启动加载 | `ConfigDirectoryLoader::Load` 沿用 `validateAndParseProtocolJson` 产物（POCO 已就绪）后追加 | 覆盖手改文件、离线导入等旁路 |

两处共用同一 helper（新文件 `src/Service/src/FrameConsistencyCheck.cpp`，声明并入 `ConfigValidator.hpp`），输入为 `Core::ProtocolConfig`（保存路径先经 `parseProtocolJson`，加载路径直接用 POCO，**零重复解析**）。

### 2.4 与长度偏移自检的关系

**互补，不替换**：长度偏移自检对"`len + 常量`"形态给出精确到偏移的差值诊断（"声明 7 ≠ 期望 8"），保留；试算校验是端到端兜底，覆盖任意表达式形态并封堵静默失败。两者都通过才放行保存。

---

## 影响文件清单

| 文件 | 变更 |
|------|------|
| `src/Engine/include/MyProt/Engine/AutoComputeProvider.hpp` | 新增 `TemplateLayout`；`ResolveDerivedLength` 签名（新增 layout 参数） |
| `src/Engine/src/AutoComputeProvider.cpp` | lookup 闭包支持 `Frame:fixed` / `Name:offset` |
| `src/Gateway/include/MyProt/Gateway/TagReader.hpp` | `TemplateWidthTable` → `BuildTemplateLayout`（或上移 Engine） |
| `src/Gateway/src/TagReader.cpp` | 布局扫描重写；`InjectDerivedLengthVariables` 上移 Engine；`WriteBytes` 调用点适配 |
| `src/Service/include/MyProt/Service/ConfigValidator.hpp` | 声明试算校验入口（POCO 入参） |
| `src/Service/src/FrameConsistencyCheck.cpp`（新） | 试算渲染 + 槽位比对 + expr 可解性检查 |
| `src/Service/src/ConfigStore.cpp` | `Validate` Protocol 分支追加调用 |
| `src/Service/src/ConfigDirectoryLoader.cpp` | Load 链路追加调用 |
| `src/Service/MyProt.Service.vcxproj`（及 filters） | 新文件登记 |
| `configs/protocols/s7-1200.json` | PDULength 迁移为 `{Frame:fixed} + {WriteValue:len}` |
| `docs/Config_Schema.md` §3.2 | 新原语文档 + `Frame` 保留名声明 |

## 测试计划

1. **单元**（Engine.Tests）：`{Frame:fixed}` / `{Name:offset}` 求值、未知格式 `hasUnknown`、layout 为 nullptr 的兼容路径；
2. **E2E 新增**：
   - S7 WriteVar 迁移后 E2E Test 14（变长写）全链路不变；
   - 篡改 S7 模板（+1 固定字节，outputs 不动）→ 保存 400，错误信息含"固定段与派生长度不一致"；
   - expr 引用不存在变量 / 语法错误 → 保存 400（原为静默 0 值）；
   - Modbus FC16 配置不变 → 试算通过（回归）；
3. **负向**：`Frame` 作占位符名 → 校验报错。

## 非目标

- 前端结构化 expr 编辑器与实时预览（后续独立提案）；
- `min/max`、条件等表达式扩展；
- 多 `{Name:raw}` 占位符的逐载荷长度语义（沿用现状：任一 raw 引用取载荷总长）；
- `{Frame:body}`（= fixedTotal − headerLength）原语：若 Modbus 迁移后 `- 6` 仍觉刺眼再议。

## 开放问题

1. 试算失败判 **error 还是 warning**？——提议 error（错误帧比拒绝保存代价高），待确认；
2. `{Name:offset}` 对**多次出现**的占位符只取首现偏移——如需按出现序号区分，留待真实需求出现再扩展。

---

## 附录 A：配置 ↔ 代码匹配核查结论（2026-09-06 全字段核对）

对 `Core::Config.hpp` 全部 POCO 字段逐个追踪消费点后，除正文所述派生长度问题外，另发现三类失配。处置如下：

### A.1 并入本 ADR 实现清单（"要么接线、要么显式告知"原则）

| 项 | 现状 | 处置 |
|----|------|------|
| `transport.defaultPort` | 校验层消费（规则 10），运行时 `TcpChannel.cpp:114` 硬编码回落 502，POCO 值从未传入 → S7 漏填 port 时**校验通过、连错端口** | `ChannelManager::PerformConnect` 在计算端点键前解析有效端口（port=0 → 协议 defaultPort），TcpChannel 的 502 退为防御性兜底 |
| ~~`tag.reportMode`/`tag.deadband`~~ | 零运行时消费，`configs/tags.json` 曾大量使用 `"OnChange"` | **字段已删除**（连带其校验与 warning）；存量配置中的该字段被解析器忽略。上报过滤见 [ROADMAP.md](../ROADMAP.md) |
| `webApi.certFile`/`keyFile` | 解析+成对校验齐全，但 WebApiServer 为自研明文 HTTP，从不读取 | warning："当前版本管理面为明文 HTTP" |
| `transport.type: Tls/Serial` | TlsChannel/SerialChannel 已实现但通道工厂仅建 TcpChannel | warning："通道工厂当前仅支持 Tcp"（README 已声明，此处补校验期信号） |

### A.2 三段校验和文法 — 已裁决：收缩文法（移除）

原问题：三段校验和文法 `{Label:crc16modbus:X4}` 在**校验器侧**文法接受且校验宽度（`isValidPlaceholder` + 规则 7），但 `RequestBuilder::RenderTemplate` 对三段直接 `BuildError`——**照文档写的配置保存成功、运行时构建必失败**，且保存期无任何信号。另：`ExecCrc` 只支持 `crc16-modbus` / `crc16-ccitt` / `crc32`，而宽度规则的 `X2` 缺省分支隐含 `lrc` / `xor8` 合法，同一条规则两处漂移。

**裁决：收缩文法**（未选「实装第二遍扫描」）。依据：三段形式全仓**零使用**，渲染器不能渲染，且声明式 `inputs.source=auto` + `strategy=crc` 已覆盖该能力——实装等于为无人使用的语法再加第二套实现。

落地：`isValidPlaceholder` 收缩为 `{Name:Xn}` / `{Name:raw}` 两段；`validateTemplate` 对三段形式报 Error（规则6）并输出迁移指引（指向 `strategy=crc`）；`validateChecksumWidths` / `IsChecksumAlgo` / `kFuncKeywords` / `extractFunctionKeyword` / `extractChecksumAlgo` / `extractFormatWidth` 一并删除。

### A.3 文档级澄清（随本 ADR 实现一并更新 Config_Schema.md）

- `responseParser.dataLengthExpr`：**仅仿真器消费**（`ResponseSynthesizer.cpp:85`），真实采集路径的解析长度恒由 `tag.registerCount × 2` 决定（超界回退取剩余全量）——Schema 需标注"仿真专用，采集不消费"，消除"动态长度表达式"的误导；
- `inputs[].label/unit/enum/placeholder`：解析进 POCO 但运行时零消费，属"UI 元信息"——Schema 标注展示用，前端动态表单消费启用；
- 代码侧测试专用：`TagGrouper::Group()` 与 `AutoComputeProvider::Reset()` 仅 E2E 测试使用，保留。

### A.4 实现清单增补

| 文件 | 增补变更 |
|------|----------|
| `src/Gateway/src/ChannelManager.cpp` | PerformConnect 入口解析有效端口（Tcp/Tls 的 defaultPort），端点键与 Connect 均用有效值 |
| `src/Service/src/ConfigDeepValidator.cpp` | A.1 四项 warning 输出 |
| `docs/Config_Schema.md` | A.3 两处标注 |

### A.5 核查中确认匹配良好的部分（不重复展开）

framing 全套、handshake 全链（含凭据注入/会话变量提取）、inputs/outputs 派生链（autoIncrement/derivedLength/expr/crc 声明式）、标签核心字段（含设备级变量回退合并 `ConfigDeepValidator.cpp:1008-1025`）、resilience 全字段、webApi 的 requireAuth/rateLimit/webRoot/bindAddress。

## 实现记录

- **2026-09-07 附录 A.1 落地**（状态：已验证）：
  - `webApi.certFile` warning 已加入 `ConfigDeepValidator`（validateWebApi）；`reportMode` / `deadband` 后来作为字段整体删除（其 warning 一并移除，见 A.1 与 A.2）；
  - `transport.defaultPort` 接线完成：`ChannelManager::PerformConnect` 入口解析有效端口（port=0 → 协议 defaultPort），端点键与 Connect 统一使用；TcpChannel 的 502 降级为防御性兜底（注释注明）；
  - `transport.type=Tls` 的运行期告警此前已存在（validateTransport L1114），无需改动；
  - 验证：全解决方案 Rebuild 通过（exit=0）；E2E 149 passed / 22 failed——**与不含本改动的基线逐条比对，失败集完全一致（均为既有环境性失败，非回归）**；生产启动 `configs/` 实测 OnChange 告警按预期输出（加载 + 热重载两路径各一次）。
- **2026-09-07 正文 ① ② 落地**（状态：已验证）：
  - **① 引擎原语**：`TemplateLayout` 结构（widths/offsets/fixedTotal/hasUnknown，`AutoComputeProvider.hpp`）；`ResolveDerivedLength` 新增 layout 参数，lookup 支持 `{Frame:fixed}` / `{Name:offset}`；`MiniExpression::ParseLenToken` 属性集扩展为 len/offset/fixed（保留原令牌文本）；`RequestBuilder::BuildTemplateLayout`（一次扫描产出布局）与 `InjectDerivedLengthVariables`（自 Gateway::TagReader 匿名实现**上移**，Gateway 与 Service 共用）公开；`TagReader::WriteBytes` 改调 Engine 实现（含 `kRawPayloadMarker`/`TemplateWidthTable` 移除）。
  - **② 试算校验**：新文件 `src/Service/src/FrameConsistencyCheck.cpp`（`ConfigValidator::CheckFrameConsistency`，POCO 入参）——派生长度 expr 可解性预检（**封堵缺口 A**：求值失败显式报错，不再静默 0 值）+ 真实渲染 + 成帧长度槽位比对 + `Frame` 保留名/未知格式拦截；挂接 `ConfigStore::Validate` Protocol 分支（深度校验通过后追加，PUT /api/config/protocols/* 即受保护）。
  - **引用域规则升级**（`ValidateDerivedLengthExprDomain` prop 感知重写）：`{Frame:fixed}` 恒合法；`{Name:offset}` 合法引用域 = 本协议全部操作模板占位符名集（协议级 outputs 的检查相应移至操作解析后，并集引用域）；`{name:len}`/裸名维持旧域。原 `CollectExprIdentifiers` 删除。
  - **配置迁移**：`configs/protocols/s7-1200.json`（PDULength → `{Frame:fixed} + {WriteValue:len}`）、`configs/protocols/modbus-tcp.json` 与 `configs_demo/protocols/modbus-tcp.json`（PDULength → `{Frame:fixed} - 6 + {WriteValue:len}`）；Config_Schema §3.2.1 已补新原语、保留名、试算校验说明。
  - **验证**：Rebuild 通过；E2E 149/22 与基线失败集逐条一致（无回归）；WebApi 实测四组用例——迁移后 S7 PUT **200**；模板加一字节 + 旧魔数 `+35` PUT **400**（帧长一致性拦截）；expr 引用 `{Typo:offset}` PUT **400**（引用域拦截）；**对照组**模板加同一字节但 expr 保持 `{Frame:fixed}` PUT **200**（自适应生效，手工同步需求消失）。
- **§2.3 裁决修订（2026-09-07）**：试算校验**仅挂接 ConfigStore::Validate（管理面保存门禁）**，`ConfigDirectoryLoader::Load` 暂不强制——存量测试/仿真配置存在语义宽松帧（如 E2E Test 15 协议长度槽位硬编码 `"00 00"`，宽容仿真器接受），加载期强制会破坏其可运行性；管理面保存路径（实际用户入口）已全覆盖。
- **§2.3 二次修订（2026-09-12）**：加载期**改为同样强制**——与保存期共用同一实现（两个加载入口 `validateConfigRootJson` / `validateAndParseConfigRootJson` 追加试算错误）。放宽的原始理由（存量宽松帧）不再成立：宽松帧由夹具侧修正（`WriteMultipleRegisters` 长度槽位 `"00 00"` → 派生 `{PDULength:X4}`，与生产配置同款），而非放宽门禁。至此手工编辑/外部生成的配置也无法绕过长度一致性校验。
- **附录 A.2（三段校验和文法）**：已裁决为**收缩文法**并落地（校验器拒绝三段 + 迁移指引；相关校验和宽度函数删除）。

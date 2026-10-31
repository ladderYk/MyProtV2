# ADR-0022：操作原型（operationProfiles）与解析期展开

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-30 |
| 日期 | 2026-10-30 |
| 相关文档 | [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md) · [ADR-0016](./0016-p2-boundary-consolidation.md) · [Config_Schema §2.3 / §2.7](../Config_Schema.md#23-operationconfigoperations-的-value) · [Config_Schema §3.4](../Config_Schema.md#34-变量就近覆盖与继承) · [04_Service §4.2](../modules/04_Service.md) |
| 提案来源 | Modbus / S7 的 8 个操作里，`outputs` 与 `responseParser` 逐条重复；S7 更是每个写操作都抄一遍 `AddrLo/AddrMid/AddrHi`。新增一条操作要复制 10~20 行相同声明，改一处公共规则要改 8 遍 |

---

## 背景

`OperationConfig` 由 `requestTemplate`（逐操作必然不同）与 `outputs` / `responseParser`（常被多个操作完全共享）组成。以迁移前的 `s7-1200.json` 为例：

| 重复项 | 出现次数 | 内容差异 |
|--------|:--:|------|
| `responseParser`（`resp[21] == 0xFF` / `dataStartIndex: 25`） | 8 个操作 | 仅读操作覆盖 `dataLengthExpr` |
| `outputs.AddrLo/AddrMid/AddrHi` | 8 个操作 | 完全相同 |
| Modbus `dataStartIndex: 9` + `dataLengthExpr: "resp[8]"` | 4 个读操作 | 仅 `validCondition` 不同 |

直接后果：

1. **漂移**：同一协议内 8 份副本，改响应解析偏移时漏改一份，错误只会在运行期表现为"某个操作读不到数"。
2. **配置体积**：S7 单文件约 500 行，其中一半以上是重复的派生长度声明。
3. **新增成本**：手工加一个 FC 需要凭记忆抄 3~4 段结构。

已有机制覆盖不到这里：§3.4 的"就近覆盖"解决的是**跨层级**同名变量合并（协议 → 操作 → 设备 → 标签），而不是同一层级内**多个操作之间**的复用。

## 决策

### 2.1 新增协议级 `operationProfiles`，只承载可复用字段

```jsonc
"operationProfiles": {
  "readResponse": {
    "description": "Modbus 读响应: ByteCount 在 resp[8]",
    "responseParser": { "dataStartIndex": 9, "dataLengthExpr": "resp[8]" }
  },
  "readBits": {
    "extends": "readResponse",
    "outputs": { "BitCount": { "source": "auto", "strategy": "derivedLength", "expr": "resp[8]" } }
  }
},
"operations": {
  "ReadCoils": {
    "kind": "read",
    "requestTemplate": ["..."],
    "responseParser": { "validCondition": "resp[7] == 0x01" },
    "profile": "readBits"
  }
}
```

允许的键**只有四个**：`extends` / `description` / `outputs` / `responseParser`。

刻意不纳入原型的字段，及理由：

| 未纳入 | 理由 |
|--------|------|
| `requestTemplate` | 每个操作的报文必然不同；允许它进原型只会催生"共享一半模板"的伪复用 |
| `kind` / `expectResponse` | 操作语义标注，与报文语义一体，复制它没有收益且容易标错 |
| `inputs` | 输入段属于"运行期取值"，其默认值链已由 §3.4 的四级作用域覆盖，再叠一层原型会让"值从哪来"多一条路径 |
| `simulation.operations` | 仿真行为按操作名独立声明，且属于部署绑定，不参与报文文法 |

### 2.2 合并顺序：父原型 → 子原型 → 操作显式字段

- `extends` 是**单继承**（一个原型一个父），沿链逐层展开。
- `outputs` 按**变量名**覆盖：整条 `VariableConfig` 替换，不做字段级合并（与 §3.4 的整条覆盖语义一致，避免"半个变量来自父、半个来自子"）。
- `responseParser` 按**字段**覆盖：可以只覆盖 `validCondition`，`dataStartIndex` / `dataLengthExpr` 继续继承。
- `description` 被子原型覆盖，仅作文档/UI 提示。

### 2.3 解析期展开，而非运行期查表

合并发生在 `ProtocolJsonParser::ParseProtocolDocImpl`：

1. 先校验全部原型（键名、允许字段、必须至少有一个 payload、`outputs` 仅 `derivedLength`、`responseParser` 字段类型与 `validCondition` 语法）；
2. 再逐个解析 `extends` 链（`visiting` 集合检测环，`resolved` 缓存消重复展开）；
3. 解析操作时，若带 `profile` 就把它当作**默认值层**铺进操作 JSON，再走原有的 `ParseOperationDoc`。

因此：

- **POCO 不变**：`Core::OperationConfig` 无需新增字段，引擎、仿真、写路径零改动；`profile` 与 `operationProfiles` 在合并后被丢弃。
- **校验顺序不变**：合并后的操作仍走同一套 `ParseOperationDoc` / `derivedLength.expr` 引用域校验 / 试渲染，不存在"走原型就跳过校验"的旁路。
- **文本边界不变**：`ConfigStore` 以 JSON 文本为操作边界（ADR-0016），管理面读写的始终是"原型 + 引用"的原始写法；展开只发生在解析方向，不会把 `operationProfiles` 折算掉再写回。

### 2.4 错误语义

| 场景 | 结果 |
|------|------|
| 原型只有 `description` / `extends`，无 payload | Error：须至少声明 `outputs` 或 `responseParser` |
| 原型出现未知键 | Error，并列出允许集 |
| `extends` 指向未定义原型 | Error：`operationProfiles 未定义: <name>` |
| `extends` 链成环 | Error：`operationProfiles.<name> 存在循环 extends` |
| 操作的 `profile` 为空串 / 未定义 | Error |
| `outputs` 为空对象 / 非 `derivedLength` | Error（与操作级 `outputs` 同规则） |
| `responseParser.validCondition` 语法错误 | Error（与操作级同用 MiniExpression 解析器） |

## 备选方案（未采用）

| 方案 | 否决理由 |
|------|----------|
| 运行期查表（`OperationConfig` 存 `profileName`，渲染/解析时再合并） | 每个消费点都要拿到原型表（`ResponseParser::Parse` 签名里没有协议对象），且把一次静态合并变成每次请求的重复工作；还会让"试渲染 / 自检"与真实路径出现两套合并实现 |
| 允许原型包含 `requestTemplate`，按行/按位置覆盖 | 模板是报文语义本身，"共享一半"没有可解释的合并单位；行号覆盖会在模板行数变化时静默错位 |
| 多继承 / mixin | 单继承已覆盖"响应格式家族"这一真实维度；多继承的菱形合并需要定义优先级，收益不抵复杂度 |
| 用 `variableAliases` 式的加载期文本重写 | 别名重写是"名字替换"，原型展开是"结构合并"；文本级合并无法正确处理 `expr` 引用域与 `validCondition` 语法校验 |
| 前端在保存时把原型展开后再落盘 | 会让磁盘配置失去复用结构，用户无法再统一修改；且绕过后端校验，JSON 模式与表单模式会出现两份真相 |

## 影响

**正面**

- S7 的 9 处 `AddrLo/AddrMid/AddrHi` 重复、Modbus 的 4 份读响应解析合并为原型；新增操作只需选原型 + 写模板 + 写差异字段。
- 公共规则（数据区起点、长度表达式）只有一份，改一处全协议生效。
- 保存期自检与试渲染直接看到展开后的结果，不需要理解原型。

**代价 / 约束**

- 读配置时需要"脑内展开"一层 `extends`；为此 WebUI 在操作卡片上直接显示**继承来源**与**展开后的只读派生输出**，并提供"覆盖 / 恢复继承"。
- 原型不参与运行期，因此运行期诊断（日志、指标）里不会出现原型名——定位时以"操作名 + 展开后的字段"为准。
- `operationProfiles` 是纯增量可选字段，不升 `schemaVersion`；旧配置（每操作自写 `outputs` / `responseParser`）继续按原语义解析，两者可混用。

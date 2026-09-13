# ADR-0005：配置版本与兼容性策略（schemaVersion）

| 字段 | 内容 |
|------|------|
| 状态 | **已接受（ACCEPTED）** — 2026-08-03（提议 2026-08-03）|
| 日期 | 2026-08-03 |
| 相关文档 | [Config_Schema.md](../Config_Schema.md) §0/§1/§4/§7/§10 · [modules/04_Service.md](../modules/04_Service.md)（加载/校验）· [ADR-0003](./0003-known-issues.md) |

---

## 背景

Config_Schema 文档标着 v4.0，但**那只是文档修订号**，JSON 配置本身**没有任何版本字段**。这带来三个问题：

- **不兼容无法被识别**：加载器无法区分"本版本配置 / 旧版/未来版配置"。字段改名（如历史决策 `pollIntervalMs → scanRateMs`、`lengthFieldSize → lengthFieldLength`）后，旧配置会被静默按新语义误读，或在校验器里报一堆零散错误，而非一条"配置版本不匹配"。
- **无迁移故事**：对一个"定义即执行、配置即产品"的网关，配置格式就是对外契约。没有版本号，就无法表达"从哪个版本升到哪个版本、哪些字段变了、如何迁移"。
- **Fail-Fast 缺抓手**：启动期校验（ConfigValidator）能查字段合法性，但查不了"整份配置是不是为这个版本写的"。

## 决策驱动因素

- **不兼容必须被一眼识别并 Fail-Fast**，不能让旧配置被新引擎静默误读。
- **与现有 schema 哲学一致**：Config_Schema §0 约定"可选字段缺省取默认、缺省不报错"，版本字段应遵循同一精神，降低手写配置摩擦。
- **机器可比较**：版本判定要简单可靠，不引入语义化版本的复杂比较。
- **v1 不做自动迁移**：迁移工具是未来项，v1 只负责"识别 + 拒绝 + 提示"。

## 备选方案

### 方案 A：语义化版本字符串（`"schemaVersion": "2.0"`）

表达力强，但需要解析与比较 major/minor/patch，且"minor 兼容、major 不兼容"的约定要靠纪律维持，加载器逻辑变复杂。

- 优点：可读、可表达渐进式演进。
- 缺点：比较逻辑复杂；对当前需求（整代不兼容判定）过度设计。

### 方案 B：整数代际号（`"schemaVersion": 1`，推荐）

一个单调递增的整数"配置代际"（generation）。加载器只认一个 `kSupportedSchemaVersion` 作**精确匹配**；任何不等都 Fail-Fast。代际号与文档修订号（v4.0）**解耦**——文档可以小步修订多次，只要配置格式不出现破坏性变化，代际号就不动。

- 优点：判定极简（一次整数比较）；语义清晰（同号=兼容，异号=不兼容）；与"可选缺省"哲学兼容。
- 缺点：无法表达"向后兼容的小增量"——但 v1 阶段所有格式变更都按破坏性处理（递增代际），增量兼容留待未来引入 `major.minor` 时再细化。

## 决策

**采用方案 B（整数代际号）**，规则如下：

### 1. 字段定义

- `schemaVersion`：整数，**配置代际号**，当前 `kSupportedSchemaVersion = 2`（代际 `1` 为 MyProtV2 首个版本化配置格式，代际 `2` 为现行字段集）。
- 归属：`ConfigRoot`（`tags.json` / 合并后的根对象）顶层**可选**字段；每个 `protocols/*.json`（`ProtocolConfig`）顶层亦可选携带。
- 与文档版本解耦：文档 `v4.0` 是规范修订号；`schemaVersion` 是机器格式代际。二者不要求相等。

### 2. 加载器判定规则（Fail-Fast）

| 情形 | 处理 |
|------|------|
| 字段缺省 | **Warning** + 假定等于 `kSupportedSchemaVersion`（遵从 §0"缺省不报错"，方便手写示例） |
| 字段存在且 `== kSupportedSchemaVersion` | 通过 |
| 字段存在且 `< kSupportedSchemaVersion` | **Error（Fail-Fast）**：`ConfigError`，message = "配置版本过旧（schemaVersion=N，本版本支持=M），请迁移后重试" |
| 字段存在且 `> kSupportedSchemaVersion` | **Error（Fail-Fast）**：`ConfigError`，message = "配置来自更高版本（schemaVersion=N，本版本支持=M），请升级网关" |
| 协议文件携带且与根不一致 | **Error**：`ConfigError`，协议文件 X 的 schemaVersion 与配置根不一致 |

> 缺省仅给 Warning 而非 Error，是为了不让文档示例与简单部署被版本字段绊住；但**生产配置应显式写明** `schemaVersion`，以获得明确的不兼容保护。

### 3. 兼容性政策

- **破坏性变更**（字段改名/删除/语义改变/必填化）→ 代际号 +1，旧配置被新加载器 Fail-Fast 拒绝。
- **非破坏性变更**（新增可选字段、新增枚举值且加载器对未知值有兜底）→ 代际号**不变**。
- **v1 不提供自动迁移**：检测到旧代际即拒绝并提示；`myprot migrate` 迁移工具列为未来扩展（见 §预留）。
- 每次代际递增须在 Config_Schema §8 迁移表追加"代际 N-1 → N 变更清单"。

### 4. 校验归属

`ConfigValidator` 在解析 JSON 之后、字段级校验之前先做版本门禁（version gate）：版本不匹配直接返回 `ConfigError`，不再继续字段校验（避免旧配置触发一堆无意义的字段错误）。

## 影响

- `Config_Schema.md`：§0 加"配置代际"约定；§1 布局注明 `schemaVersion` 位置；新增 `ConfigRoot.schemaVersion` 与 `ProtocolConfig.schemaVersion` 字段说明；§7 增版本门禁校验项；§8 迁移表增"代际变更清单"占位；§10 同步项。
- `modules/01_Core.md`：`ConfigRoot` / `ProtocolConfig` POCO 加 `int schemaVersion`（缺省 = 当前代际）。
- `modules/04_Service.md`：`JsonConfigLoader` / `ConfigValidator` 增版本门禁步骤（先于字段校验）。
- 文档所有 JSON 示例：建议（非强制）写 `"schemaVersion": 1`。

## 预留扩展

- `myprot migrate --from N --to M`：按代际差异表自动改写旧配置（v1 不做）。
- 引入 `major.minor` 双层版本以表达"向后兼容增量"（当出现频繁的非破坏性演进需求时）。

## 复核触发条件

- 出现需要"同一版本同时兼容两代配置"的灰度升级需求（说明精确匹配策略需放宽）。
- 非破坏性变更频繁到"代际号长期不动但格式漂移严重"，需要更细的兼容矩阵。

---

> **相关文档**: [Config_Schema.md](../Config_Schema.md) · [ADR-0003](./0003-known-issues.md) · [文档索引](../README.md)

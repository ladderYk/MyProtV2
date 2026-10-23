# ADR-0015：Service 对 Engine 的依赖方向（保留并约束）

| 项 | 内容 |
|----|------|
| 状态 | **已接受（ACCEPTED）** · 2026-10-23 |
| 日期 | 2026-10-23 |
| 相关文档 | [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md)（模板结构原语与保存期试算）· [modules/02_Engine](../modules/02_Engine.md) · [modules/04_Service](../modules/04_Service.md) · [architecture/02](../architecture/02_Layered_Architecture.md)（依赖方向） |
| 提案来源 | 架构评审 P1-4：Service 直接依赖 Engine 的表达式/渲染原语，需在"下沉到 Core"与"保留并约束"之间二选一 |

---

## 背景

Service 的加载期校验需要与运行时**完全一致**的两类协议语义：

| 校验点 | 位置 | 复用能力 |
|--------|------|----------|
| `validCondition` / `derivedLength` 语法 | `ConfigDeepValidator.cpp` | `Engine::MiniExpression::Parser` |
| 写帧试算（成帧长度槽位比对） | `TrialRenderer.cpp` | `Engine::RequestBuilder` / `AutoComputeProvider` |

因此 `MyProt.Service` 静态库链接 `MyProt.Engine`。评审提出的替代方向是把
「渲染/表达式能力」下沉到 Core，使 Service 只依赖 Core。

## 决策

**保留 `Service → Engine` 依赖，并以 ADR 约束其使用边界。**

理由：

1. **语义归属**：模板渲染、派生长度、表达式求值都是"协议如何成帧"的语义，
   按 [ADR-0012](./0012-derivedlength-template-primitives-and-trial-render.md)
   已明确置于 Engine；Core 保持领域中立（POCO / Expected / Optional /
   ByteView / 模板文法解析）。
2. **单一实现**：加载期与运行时共用同一解析器/渲染器，是"保存即运行"的
   前提；任何下沉都会面临"整体搬迁"或"两套实现漂移"。
3. **搬迁收益不足**：`RequestBuilder` 依赖 `TemplateGrammar`、`MiniExpression`、
   `AutoComputeProvider` 与 `OperationConfig`，整体搬到 Core 等于把协议引擎
   改名为 Core，分层反而失真。

约束：

- Service 只可调用 Engine 的**无状态校验/渲染原语**，不得调用编排、I/O 或
  通道相关 API（当前无此类调用）。
- 新增 Service→Engine 的调用点须在 PR 中说明"为何运行时也必须使用同一语义"，
  并更新本节清单。
- 依赖图必须保持无环：`App → {Gateway, Polling, Service, WebApi}`，
  `Service → Engine → Core`，`Gateway → {Engine, Transport, Service}`，
  `Polling → {Gateway, Engine}`。

## 备选方案与取舍

| 方案 | 否决原因 |
|------|----------|
| 将 `MiniExpression` 移至 Core | 可行但收益有限：只解决表达式，试算渲染仍依赖 Engine；两处 include 迁移会制造一次性大 diff |
| 将 `RequestBuilder` / `AutoComputeProvider` 移至 Core | Core 被迫承载协议帧语义，`OperationConfig` 等 POCO 也成为 Core 的公共契约，分层边界后退 |
| Service 内复制一套渲染/表达式 | 必然漂移，违背 ADR-0012 的单一实现原则 |

## 影响

- 构建：`MyProt.Service` 的链接依赖为 `Core + Engine + nlohmann/json`；
  CMake 与 `.vcxproj` 均须显式列出（当前已满足）。
- 文档：模块依赖声明必须如实写 `Core + Engine`，不得再写"仅 Core"。

## 复审触发条件

- Core 出现**领域中立的通用表达式设施**，且 Service 不再需要试算渲染；
- 出现 Engine 与 Service 的循环依赖，或 Service 需要在无 Engine 的裁剪包中交付；
- Engine 被拆分/下沉，`RequestBuilder` 的归属发生变化。

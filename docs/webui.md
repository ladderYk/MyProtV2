# WebUI 使用指南

基于 Vue 3 + Element Plus 的配置与监控前端，位于 `webui/`。

## 启动

```bash
cd webui
npm install
npm run dev        # 开发模式, 代理 /api → 127.0.0.1:8080
npm run build      # 产物 dist/, base='./' 支持子路径部署
```

默认开发配置（`requireAuth=false`）直接进入；启用鉴权时首次打开需输入访问令牌（与后端 WebApi 一致）。

## 回归测试

```powershell
powershell -ExecutionPolicy Bypass -File scripts\webui-smoke.ps1
powershell -ExecutionPolicy Bypass -File scripts\webui-smoke.ps1 -SkipUiBuild   # 复用已有 dist
```

脚本会重建 `dist/`、在空闲环回端口启动 `MyProt.App`、用无头 Edge 的 DevTools Protocol 跑 75 项断言（顶栏与响应式、认证关闭时的入口形态、总览、帮助抽屉、协议与标签编辑、未保存守卫、实时监控与写入抽屉、仿真器），并检查 console 无错误、无 429、无失败请求。`scripts/ci.ps1` 默认在 E2E 之后执行该脚本，`-SkipWebUI` 可跳过；需要 Node.js（除非 `-SkipUiBuild`）与 Microsoft Edge。

## 导航与页面结构

顶部导航按任务分组，默认落地 **总览**；之后会在 `localStorage` 记住上次访问的页面（`mp.ui.view`）。

| 分组 | 页面 | 功能 |
|------|------|------|
| 总览 | 总览 (Overview) | 状态卡、开始使用（清单/就绪摘要）、配置健康、设备概览 |
| 运行 | 实时监控 (Live) | SSE 实时值、迷你趋势图、写入抽屉 |
| 运行 | 仿真器 (Sim) | 寄存器读写、仿真配置持久化 |
| 运行 | MQTT 测试 (Mqtt) | 网关代连 broker 订阅过滤器，实时观察北向发布 |
| 配置 | 协议配置 (Protocols) | 协议 JSON 的表单/双模编辑 |
| 配置 | 设备与标签 (Tags) | 设备树与标签的表单/双模编辑 |

顶栏右侧固定为帮助抽屉入口与退出登录。

顶栏承载品牌、分组导航、全局状态（设备在线数、标签 Good/Bad）、手动热重载与帮助入口；页面级统计不再重复到顶栏。顶栏使用深色底（`--mp-topbar-*`），与浅色工作区形成层次；导航在窄屏下保持单行并可横向滚动，品牌与状态条按宽度逐步收敛，内容区始终占满剩余高度。

## 总览与首次使用

总览页是首次配置的主入口，不使用强制遮罩或分步弹窗：

- **状态卡行**：设备在线/总数、标签 Good/总数、协议数、仿真器数，点击可跳到对应页面。
- **开始使用**（0/4）：未就绪时展开四步清单（至少 1 个协议 → 至少 1 个设备 → 至少 1 个标签 → 至少 1 个 Good 值），每步都带一键跳转；全部完成后自动折叠为一行「配置已就绪 · 设备/标签/Good 摘要」，避免已完成项长期占据首屏。已配置但无 Good 时提示检查连接或启动仿真。
- **配置健康**：标签配置的校验错误/警告数与最近一次热重载结果，内联在「开始使用」区，不再单独占一块。
- **设备概览表**：设备 ID、协议、标签数、Good/Bad、最后更新时间，点击行定位到该设备。

运行状态统一由 `useRuntimeStatus` 每 5 秒轮询（页面隐藏时暂停），顶栏、总览与实时监控共用同一份数据，避免重复请求触发限流。

所有空状态（协议列表、监控表、仿真器、变量表）都提供一句解释 + 一个主操作，例如「还没有协议 → 新建 Modbus TCP 模板」。

## 设计系统

- `webui/src/styles/tokens.css`：自研组件的色值/度量来源（`--mp-*`），与 Element Plus 默认视觉对齐（默认蓝、4px 圆角、中性灰阶）；深色顶栏单独使用 `--mp-topbar-*` 一组 token。
- Element Plus 使用官方默认样式，不再维护主题覆盖层（原 `element.css` 已移除）；自研组件已与默认蓝对齐，如需换主色需同时覆盖 `--el-color-primary`。
- `webui/src/style.css`：全局基础与 `.btn` 按钮体系（`primary` / `danger` / `ghost` / `small` / `tiny`）。

新组件不应再硬编码色值；数值与标识符（协议名、操作名、变量名、表达式、寄存器值）统一使用 `--mp-font-mono`。

## 交互约定

- 长表单页使用 `PageHeader`（标题 / 说明 / 状态 / 右侧动作）与吸顶工具栏；协议页与标签编辑的段内页签为吸顶分段控件。
- 设备与标签页的设备视图不再使用页签：基本信息与连接参数同屏展示，标签列表由左侧设备树承担，避免右侧重复一份标签表。
- 仿真寄存器表的十六进制值内联在当前值右侧，不再单独占一列。
- 实时监控的写入改为右侧 420px 抽屉，保留原字段与校验，不遮挡监控表格；表头吸顶、数值右对齐、质量徽标统一。
- 协议配置的校验问题为可折叠横幅：默认显示「错误 N / 警告 M」，展开后点击条目可跨段定位。
- 历史备份收进下拉菜单；编辑区继续使用 `v-show` 复用表单/JSON 双模实例，切换不丢选中态。
- JSON 模式为深色代码面板，与浅色表单形成明确的专家模式对比。
- 帮助抽屉（顶栏「帮助」）包含快速上手 4 步、当前页面要点、术语速查与校验说明。

所有配置保存（`PUT /api/config/{scope}/{name}`）成功后自动触发热重载；也可手动 `POST /api/config/reload`。

## 实时监控展示约定

- 整张表只有一个表头；设备名作为分组行插入表体，右侧显示该组最近更新时间，点击分组行可折叠该设备。
- 趋势列仅在已有 2 个以上采样点时出现，避免无数据时整列为 `-`。
- 名称列优先显示标签配置的 `label`（中文/显示名），下一行以等宽小字显示全局唯一的 `name`；未配置 `label` 时只显示 `name`。
- 数值列在值后追加标签配置的 `unit`（如 `%`、`°C`）；离线或无值（`value=null`）时不显示单位。
- 搜索框同时匹配 `name`、`deviceId` 与 `label`，可直接输入中文名过滤。
- `label` / `unit` 仅为展示元数据，不参与采集与数据面协议；字段定义见 [Config_Schema.md §5](./Config_Schema.md)。

## 校验可见化（保存前自检 / 保存被拒解释）

配置保存要过服务端深度校验（帧长一致性、引用完整性等），不通过即拒绝并回滚。为了让"改了就弹回"不再无迹可寻：

- 工具栏 **「自检」**：不落盘，对当前编辑器内容跑同一套校验，直接给出结构化问题清单
- **保存被拒时**：除顶部横幅的纯文本原因外，自动把同一份结构化清单渲染成**可折叠横幅**（默认只显示计数，展开为问题列表）
- **点击条目跳转**：每条带 `ruleId` / 主体（操作名、设备 ID、标签名）/ 建议定位路径；点击后自动切回「表单」模式并跳到对应位置：
  - 协议：`operations.*` → 「操作定义」段并选中该操作；`outputs` / `variables` → 「变量」段；其余 → 「基础信息」段
  - 标签：`devices.<id>` → 选中该设备；`tags.<name>` / `ref.tag*` → 选中该标签

对接的只读端点（详见 [modules/07_WebApi.md §7.3.1](./modules/07_WebApi.md)）：`GET /api/validate` 校验磁盘现值、`POST /api/validate` 校验候选内容；响应 `issues[]` 形如 `{severity, ruleId, subject, field, message}`，`warning` 不阻断保存。

## 试渲染预览（不启动网关也能看到请求字节）

协议配置页工具栏的 **「试渲染」** 把当前编辑器内容（含未保存修改与 `//` 注释）+ 选中的操作 + 变量覆盖，渲染成一帧真实请求并逐字节展示：

- **只读**：不写盘、不触发热重载、不连任何设备；与网关共用同一条渲染管线（`RequestBuilder` + 派生长度注入 + autoCompute），因此看到的就是上线后会发出的字节。
- **变量**：默认值按「协议 `inputs` → 操作 `inputs`」合成，并补上引擎契约量（`StartByteAddress` / `ByteCount`）；`source=auto` 的变量（自增序号、帧切片、CRC 等）在渲染期求值，表格中显示为「渲染期求值」。
- **raw 载荷**：面板会在标签旁列出当前操作模板里的 `{Name:raw}` 占位符名，并在输入框内给出对应 JSON 示例（如 `{"Data": "01 02 03"}`）；显式给 hex 即按给定字节渲染，省略时按「缺省载荷字节数」生成 `01 02 03…` 的占位载荷，用来验证 `derivedLength` 是否把长度算对。
- **长度槽位**：若成帧声明了 `lengthField`，面板会交叉核对槽位值与期望值，不一致时红标并附原因——这正是保存期「帧长一致性」校验失败时最直接的定位手段。注意两处口径不同：**本面板对所有 `LengthField` 操作核对**，而保存期只核对含 `{Name:raw}` 的写操作，因此可能出现「预览报红、保存却通过」——以预览为准（读操作同样会发出错误帧）。
- 支持 **复制 hex**，便于粘进 Wireshark / Modbus Poll 做对照。

后端端点 `POST /api/preview?scope=protocols`（`GET` 走磁盘现值），响应 `{ok, hex, ascii, frameLength, errors[], warnings[], variables[], lengthField|null}`；语义与 `/api/validate` 一致：请求成功处理即 `200`，语义结果看 `ok`。

### 响应解析（反向：响应字节 → 值）

同一抽屉的 **「响应解析」** 页签是试渲染的反向操作：粘贴设备实际返回的 hex，看当前编辑器内容能否把它解析成预期值。

- **同一解析器**：走 `Engine::ResponseParser`，因此 `dataStartIndex`、字节序、`finalType` 宽度、`bitOffset` 位提取的语义与网关逐位一致。
- **输入**：响应 hex（空格/换行/逗号可省略）+ 数据类型（`UInt16`…`ByteArray`）+ 位偏移（仅 `Bool` 有意义，`-1` = 不提取）+ 字节序覆盖（缺省用协议/标签级解析顺序）+ 数据字节数（`0` = `dataStartIndex` 之后剩余全量）。
- **结果**：解析值卡片（类型 / 值 / 质量码徽标）、`validCondition` 是否为真、`dataStartIndex`、帧长、数据区字节，以及 `byteCount` 超出数据区时的退化警告。
- **标签换算（可选）**：下拉列出 `tags.json` 中声明了 `converters` 的标签，选项本身带 `×k + b` 摘要（如 `Tank.PV · plc-1  ×0.1 - 40`），选中即按该标签口径解析（操作 / `finalType` / `bitOffset` / `ByteCount` 自动对齐），结果区同时显示「换算前 → 换算后」与实际换算链（`×k + b`，按序应用，结果统一为 `Double`）。若该标签的 `operation` 不在当前协议中，面板给出提示，仅套用类型与换算链、不切换操作。
- **结果时效**：修改 hex / 数据类型 / 位偏移 / 字节序 / 数据字节数 / 已选标签或切换操作后，上一次的解析结果立即清空，避免与当前输入不符的结果残留。
- 校验失败（条件不满足 / 数据不足 / hex 非法）时面板红标并给出原因，**不给一个看似正常的值**。

后端端点 `POST /api/preview/response?scope=protocols`（`GET` 走磁盘现值；`hex` 含空格时须用 POST，因为路由不做 URL 解码），响应 `{ok, condition{checked,expr,ok}, byteOrder, dataStartIndex, frameLength, rawLength, rawHex, value{type,text,numeric}|null, converters[], valueBeforeConversion{type,text,numeric}|null, quality, errors[], warnings[]}`。

## 三个可视化组件

### FrameBuilder — 帧构建器（协议配置 → 请求模板）

把 `requestTemplate` 的 hex 段编辑从"手写十六进制串"变成字段表格：

- 每行一个片段：**hex 字面量** / **变量占位符** / **raw 载荷**
- 变量行可选格式（`X2`/`X4`/`X8`/`raw`），输入变量名时自动补全（来源：协议 inputs + 当前操作 inputs）
- 内置模板预设：S7 读/写、Modbus 读/写，一键起步
- 可视化 ↔ 原文双 Tab 实时互转，非法片段保存前即报错

### ExprBuilder — 表达式构建器（协议配置 → outputs 派生字段）

为 `derivedLength.expr` 提供可视化拼装：

- 运算符按钮（`+ - * / % ( )`）+ 变量下拉引用（含 `{Var:len}`、`{Frame:fixed}` 特殊引用）
- 片段库一键插入：S7 地址拆分、Modbus 长度计算等常用表达式
- 输入当前上下文变量值即可**实时求值预览**（如 StartByteAddress=1000 → AddrMid=31）
- 表达式 ↔ 表单双模式，最终仍以 `expr` 文本落盘

### VarTracePanel — 变量来源追踪（标签配置 → 每个标签卡片）

回答"这个变量的值最终是哪来的"：

- 按 **协议默认 → 操作默认 → 标签 variables → 标签 writeVariables** 四层展示覆盖链
- 生效值高亮显示来源层；悬停可看各层原始值
- 点击某层的编辑按钮自动滚动定位到对应编辑区（协议层会提示前往协议配置页）

## 双模编辑约定

所有复杂字段（模板、表达式、整个配置文档）都遵循同一模式：**表单模式适合日常修改，JSON/原文模式保留完全控制力**，两种模式实时同步、切换不丢数据。

## 新建协议的起点

左下角「新建协议」的模板下拉分两组：

- **内置模板**：Modbus TCP / S7-1200（含握手）/ SEER AGV / 空白骨架 —— 内容随前端发布，适合从零起步。
- **复制现有协议**：列出 `configs/protocols` 下的全部协议（含 FINS / MC-3E / ADS 等），选中后读取**磁盘现值**作为起点、写入新名称，并把 `simulation.listenPort` 重置为 `0`（避免与源协议抢同一仿真端口）。原协议的未保存修改不会被复制。

复制走的是普通保存链路（深度校验→备份→原子写入→自动热重载），因此模板本身必须是一份能通过校验的配置。

## 北向 MQTT 测试面板

用于端到端验证北向 MQTT 发布（`server.json` 的 `mqtt` 段驱动的 `MqttPublisher`）。

浏览器的 WebSocket API 无法连接原始 TCP 1883，因此面板**不再直连 broker**：表单把连接参数 `POST /api/mqtt/probe` 交给网关，网关用北向 `MqttClient`（TCP）连接 broker、`SUBSCRIBE` 指定过滤器，再把消息与连接状态经 SSE（`/api/mqtt/stream?sid=...`）批量回推。**broker 无需开启 WebSocket 监听**。

- **连接配置**：Broker 主机（默认 `127.0.0.1`）、端口（默认 `1883`）、订阅过滤器（默认 `myprot/#`）、QoS（0/1）、Client ID（留空自动生成）、心跳秒、可选用户名/密码。
- **「开始监听」**：建会话 → 打开 SSE 流；状态徽标由后端推送的 `state` 驱动（`connecting` / `online` / `error` / `closed`）。
- **「停止监听」**：关闭 EventSource 并 `DELETE` 会话；关闭页面或离开本页同样自动断开。
- **消息表**：时间/主题/QoS/retain/载荷，最多 500 条，可暂停/清空；非法 UTF-8 或超 8 KB 的载荷以**十六进制**呈现并打「已截断」标记。
- **连接日志**：连接状态变更、会话失效、发布失败等事件，最多 200 条。
- **发布测试消息**：借同一会话 `PUBLISH` 一条自定义主题/载荷，若匹配订阅过滤器会立即回显，用于回环自检；会话未 `online` 时按钮禁用（后端返回 `409`）。
- **偏好持久化**：`localStorage` 的 `mp.ui.mqtt` 保存主机/端口/过滤器/QoS/Client ID/用户名/心跳，**不保存密码**。

实现：后端 `src/Northbound/src/MqttProbeHub.cpp`（会话/事件/回收/校验）+ `src/App/MqttProbeApi.cpp`（HTTP 适配），前端 `webui/src/components/MqttTestPanel.vue` 只消费 REST + EventSource，无新增前端依赖。

**边界与限额**：凭据只驻内存（不写配置、不记日志）；最多 8 个并发会话；30 秒无推流自动回收；网关重启后全部会话失效（面板提示重新开始）；TLS（8883）不在范围内。

启动 `mosquitto` 并把 `server.json` 的 `mqtt.enabled` 置 `true`（`broker` 指向 `127.0.0.1:1883`），网关有变化的标签后，在本面板填 `127.0.0.1:1883` 订阅 `myprot/#` 即可看到消息。

> 若只想验证发布逻辑而无外部 broker，可跑 `MyProt.Northbound.Tests`（进程内假 broker 的端到端断言，含 `MqttProbeTests`）。

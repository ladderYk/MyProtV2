# 协议教学文档

> 本目录面向**已掌握协议规范、想了解 MyProt 配置如何映射到协议字节**的读者。
>
> 主文档 [docs/README.md](../README.md) 介绍 MyProt 整体架构与文档维护原则；本目录专注"配置 → 字节"对照。

## 阅读路径

1. 已有 Modbus 基础 → 直接看 [modbus-tcp.md](./modbus-tcp.md)
2. 接西门子 S7-1200/PLC → 看 [s7-1200.md](./s7-1200.md)
3. 接仙工（SEER）AGV 控制器 → 看 [seer.md](./seer.md)
4. 想加新协议 → 看本 README 的"如何新增一个协议"段
5. 想理解配置 schema → 回 [Config_Schema.md](../Config_Schema.md)

## 已支持协议

| 协议 | 配置 | 教学文档 | 状态 |
|------|------|----------|:----:|
| Modbus TCP | [configs/protocols/modbus-tcp.json](../../configs/protocols/modbus-tcp.json) | [modbus-tcp.md](./modbus-tcp.md) | ✅ v1 |
| Siemens S7-1200 (S7comm/ISO-on-TCP) | [configs/protocols/s7-1200.json](../../configs/protocols/s7-1200.json) | [s7-1200.md](./s7-1200.md) | ✅ v1（仅 Read/Write Var；无内置仿真器）|
| 仙工智能 SEER AGV（0x5A 01 + JSON 正文）| [configs/protocols/seer.json](../../configs/protocols/seer.json) | [seer.md](./seer.md) | ✅ v1（正文需 hex 编码；无内置仿真器）|

## 如何新增一个协议（指引）

> 完整步骤不写在这里——见 [ROADMAP.md §3 评审流程](../ROADMAP.md) 与 [adr/](../adr/) 现有决策。

最小步骤：

1. **写协议 JSON**：`configs/protocols/<name>.json`
   - 参考 modbus-tcp.json 的结构：metadata + operations
   - 每个 op 必须有 `requestTemplate` + `responseParser.validCondition/dataStartIndex/dataLengthExpr` + `placeholderHints`
2. **写教学文档**：`docs/protocols/<name>.md`
   - 帧结构表 + 配置↔字节映射 + 异常处理
3. **注册协议插件**（仅当 PDU 长度需运行时计算）：见 [modules/05_Gateway.md §5.3](../modules/05_Gateway.md#53-写路径)
4. **加 E2E 测试**（可选但推荐）：仿真器 → MyProt → LatestValueStore → WebUI

## 协议模板通用约定

### 占位符语法（详见 [Config_Schema.md §2.4](../Config_Schema.md#24-占位符模板)）

| 形式 | 含义 | 典型用途 |
|------|------|----------|
| `{Name:X4}` | 16-bit 大端无符号 | 寄存器值 |
| `{Name:raw}` | 原样 hex 字符串 | 变长写 |
| `{TransactionID:auto:X4}` | 自动递增 16-bit | Modbus TCP TransactionID |
| `{PDULength:X2}` | PDU 长度（注册式策略） | Modbus FC15/FC16 长度字段 |

### responseParser 通用约定

- `validCondition`：判响应合法性的 Mini-C 表达式；`resp[N]` 表示 N 字节偏移
- `dataStartIndex`：数据区起点偏移
- `dataLengthExpr`：数据区长度计算式

## 异常处理通用约定

- **MyProt 端的"异常响应"语义** = `validCondition` 不通过
- 不会自动解析 Modbus 异常码（0x01~0x0B），**只判 valid**
- 需要细化异常处理时，可在 `validCondition` 里加 `|| resp[7] == 0x83` 等判异常分支

## 排错通用路径

```
[Data] quality=Bad
   ↓
看 [Channel] 标签: 连接/超时/RST
   ↓
看 [Data] 后续: responseParser 是否报"条件不通过"
   ↓
仿真器侧看 raw 响应: 用 Wireshark 过滤 tcp.port == <protocol-port>
   ↓
对照本目录教学文档的"帧结构表"查错
```

## 关联索引

- 主架构: [02_Layered_Architecture.md](../architecture/02_Layered_Architecture.md)
- 配置规范: [Config_Schema.md](../Config_Schema.md)
- 扩展候选: [ROADMAP.md](../ROADMAP.md)

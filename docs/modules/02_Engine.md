# MyProtV2 ▸ Engine 模块

> **所属**: MyProtV2 模块设计系列
> **上一篇**: [Core 模块](./01_Core.md)
> **下一篇**: [Transport 模块](./03_Transport.md)

---

**依赖**: 仅 `Core`

> ⚠️ 配置相关契约（模板文法、字段语义）以 [Config_Schema.md](../Config_Schema.md) 为准。
>
> **v4 注记**：本模块为**纯具体类工具库**（无编排器、无接口层）。早期设计的 `ProtocolEngine` 编排类、`IExpressionEvaluator` / `IRequestBuilder` / `IResponseParser` 接口体系及其工厂已删除——管线装配职责收敛于 Gateway 层的 `TagReader`（见 [05_Gateway.md](./05_Gateway.md)），Engine 只提供可独立调用的构造/解析/求值/转换原语。

## 模块组成

| 头文件 | 内容 |
|--------|------|
| `ExpressionEvaluator.hpp` | 表达式求值器（递归下降）|
| `RequestBuilder.hpp` | 请求模板展开器 |
| `ResponseParser.hpp` | 响应校验、数据提取、字节序裁决 |
| `AutoIncrementProvider.hpp` | `{Name:auto:Xn}` 自增计数器 |

## 2.1 表达式语法 (EBNF)

```
expression    := logical_or

logical_or    := logical_and ( "||" logical_and )*
logical_and   := bitwise_or ( "&&" bitwise_or )*
bitwise_or    := bitwise_xor ( "|" bitwise_xor )*
bitwise_xor   := bitwise_and ( "^" bitwise_and )*
bitwise_and   := equality ( "&" equality )*
equality      := relational ( ("==" | "!=") relational )*
relational    := shift ( ("<" | ">" | "<=" | ">=") shift )*
shift         := additive ( ("<<" | ">>") additive )*
additive      := multiplicative ( ("+" | "-") multiplicative )*
multiplicative:= unary ( ("*" | "/" | "%") unary )*
unary         := ("!" | "-" | "~") unary | primary
primary       := NUMBER | HEX_NUM | "resp" "[" index_expr "]" | "(" expression ")"
index_expr    := NUMBER | NUMBER ":" NUMBER    # resp[5:9] = bytes 5~9 (inclusive)
NUMBER        := [0-9]+
HEX_NUM       := "0x" [0-9a-fA-F]+
```

> **优先级已对齐 C 语言（KI-13）**：从松到紧为 `||` < `&&` < `|` < `^` < `&` < `== !=` < `< > <= >=` < `<< >>` < `+ -` < `* / %` < 一元 `! - ~`。
> 注意：`==` 比 `|`/`&` 更紧（与 C 一致），因此"先按位组合再比较"**必须显式加括号**，如 `((resp[0]<<8)|resp[1])==0x0016`；若写成 `(resp[0]<<8)|resp[1]==0x0016` 会被解析为 `(resp[0]<<8)|(resp[1]==0x0016)`，语义错误。
>
> **v1 范围**：仅支持 `resp` 字节流作用域，不支持会话变量引用。

支持示例：

```
"resp[1]==0x03"                    // 单字节判断
"resp[5]==0xD0"                    // S7 COTP 握手验证
"((resp[0]<<8)|resp[1])==0x0016"   // 两字节组合判断 (== 比 | & 须加括号)
"resp[2]>=4"                       // 长度判断
"resp[4]*256+resp[5]"              // 双字节拼出长度
```

## 2.2 ExpressionEvaluator — 递归下降求值器

```cpp
// src/Engine/include/MyProt/Engine/ExpressionEvaluator.hpp

namespace MyProt { namespace Engine {

/// 表达式求值器
/// 支持 resp[N] 字节引用, resp[A:B] 切片, 比较运算, 算术/逻辑运算
class ExpressionEvaluator {
public:
    /// 求值条件表达式 (如 "resp[7] == 0x03")，用于 validCondition
    Core::Expected<bool> EvaluateCondition(
        const std::string& expr,
        Core::ByteView response);

    /// 求值长度表达式 (如 "resp[2]")，用于 dataLengthExpr
    Core::Expected<int> EvaluateLength(
        const std::string& expr,
        Core::ByteView response);

private:
    // 词法: Tokenize 产出 Token 流 (Number / RespIndex / RespRange / 括号 / 运算符)
    // 语法: 递归下降, 每优先级一个 ParseXxx 方法, 返回 int64_t (布尔非零 = true)
};

}} // namespace MyProt { namespace Engine
```

实现要点：词法分析产出 Token 流后由逐级下降的 `ParseOr → ... → ParsePrimary` 求值，优先级结构与 §2.1 EBNF 一一对应；越界索引、除零、非法 token 均返回解析错误。

## 2.3 RequestBuilder — 模板展开

```cpp
// src/Engine/include/MyProt/Engine/RequestBuilder.hpp

namespace MyProt { namespace Engine {

/// 无状态模板展开器 (E2E/Gateway 直接调用)
///
/// 模板片段语法定义:
///   "0A1B"             → 十六进制字面量 (每 2 字符 = 1 字节)
///   "{Name:X4}"        → 变量为 4 位十六进制 (= 2 字节) 大端输出
///   "{Name:auto:X2}"   → AutoIncrementProvider 自增计数 (Xn 即 n/2 字节)
class RequestBuilder {
public:
    /// 展开操作模板生成请求字节流
    /// @param op 操作配置 (requestTemplate 为模板片段序列)
    /// @param variables 标签变量集 ({ "StartAddress": 0, ... })
    /// @param autoProvider 自增计数器 (供 {Name:auto:Xn} 使用)
    /// @return 请求字节流，失败返回 BuildError
    Core::Expected<Core::Bytes> Build(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        AutoIncrementProvider& autoProvider);
};

}} // namespace MyProt { namespace Engine
```

> **v1 边界**：构建器为单遍处理，仅支持上述三种片段。`{L:calc:Xn}` 等 calc 占位符**未实现**——模板中出现即报 `BuildError`（校验器规则 7 "使用未声明函数"，E2E 有对应断言）。长度字段由标签变量直接给出（如 Modbus TCP 用 `RegisterCount` 配合批量合并逻辑）。

## 2.4 AutoIncrementProvider — 自增计数

```cpp
// src/Engine/include/MyProt/Engine/AutoIncrementProvider.hpp

namespace MyProt { namespace Engine {

/// 全引擎共享的原子自增计数器 (std::mutex 保护)
/// 每个计数器名独立维护, 按格式宽度取模 (X2→mod 256, X4→mod 65536, ...)
class AutoIncrementProvider {
public:
    uint64_t Next(const std::string& name, int byteWidth);  // 取下一个, mod 2^(8*byteWidth)
    void Reset(const std::string& name);                    // 重置指定计数器 (测试用)
    void ResetAll();
private:
    std::unordered_map<std::string, uint64_t> _counters;
    std::mutex _mutex;
};

}} // namespace MyProt { namespace Engine
```

调用方（Gateway 单读 / 批量读 / 写路径）共享同一实例，保证同设备事务 ID 全链路唯一递增。

## 2.5 ResponseParser — 响应解析

```cpp
// src/Engine/include/MyProt/Engine/ResponseParser.hpp

namespace MyProt { namespace Engine {

/// 解析流程:
///   1. validCondition 校验 (支持 "resp[N]==V", V 为十进制或 0x 十六进制)
///   2. dataStartIndex 定位数据区
///   3. registerCount×2 字节 (不足则取剩余全量) 按 finalType 转换
///   4. 产出 TagValue (quality=Good, timestamp=now)
class ResponseParser {
public:
    /// 字节序裁决：tag 级覆盖 > 协议 dataByteOrder > 默认大端
    /// (单读/批量读等所有管线的唯一裁决点，避免各处重复三段式回退逻辑)
    static Core::ByteOrder ResolveByteOrder(
        const Core::ProtocolConfig& protocol,
        const Core::Optional<Core::ByteOrder>& tagOverride);

    /// 校验 validCondition 子集 "resp[N]==V"
    /// (写应答 echo 等只需条件校验、不解析数据区的场景)
    static bool CheckCondition(const Core::ByteView& response,
                               const std::string& validCondition);

    /// 解析响应并转换为标签值
    /// @return 标签值，失败返回 InvalidResponse/ParseError/TypeConversionError
    Core::Expected<Core::TagValue> Parse(
        const Core::ByteView& response,
        const Core::ResponseParserConfig& config,
        const Core::TagDefinition& tag,
        Core::ByteOrder byteOrder);
};

}} // namespace MyProt { namespace Engine
```

`Parse` 内部按 finalType 直接调用 `Core::FromBytesU16/U32/U64` 等字节序装配函数，整型按指定字节序解释，浮点字序装配后按 IEEE 754 位拷贝解释（C++11 `memcpy`），`Bool` 取首字节非零，`String` 直拷原始字节，并按 ADR-0006 映射质量码；`rawData` 在 Uncertain 时保留原始字节供诊断。

---

> 尚未实装的扩展项统一登记在 [ROADMAP.md](../ROADMAP.md)。
> **上一篇**: [Core 模块](./01_Core.md)
> **下一篇**: [Transport 模块](./03_Transport.md)

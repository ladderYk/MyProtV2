// src/Engine/include/MyProt/Engine/RequestBuilder.hpp
// 请求构建器 — 模板化请求生成 (C++11, ADR-0010 §5)

#pragma once
#include <string>
#include <unordered_map>
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"

namespace MyProt { namespace Engine {

// ────────── 具体构建器 — 无状态模板展开 (E2E/Gateway 直用) ──────────

/// 模板片段语法定义:
///   "0A1B"             → 十六进制字面量 (每 2 字符 = 1 字节)
///   "{Name:X4}"        → 变量按 4 位十六进制 (= 2 字节) 大端输出
///   "{Name:Xn}"        → 是否 auto 由 autoCompute 声明决定 (不接受 :auto: 令牌)
///   "{Name:raw}"       → 变长字节注入 (查 variableBytesHex 表; 契约见 ADR-0007 §3)
class RequestBuilder {
public:
    /// 展开操作模板生成请求帧 (标量变量路径)
    /// @param op 操作配置 (requestTemplate 为模板片段序列)
    /// @param variables 标签变量表 ({ "StartAddress": 0, ... })
    /// @param varAliasMap 变量别名映射 (alias → internal name); 空映射 = 无别名
    /// @param autoProvider 声明式 auto 求值器 (供 autoCompute 段声明的变量使用)
    /// @return 请求字节流; 失败返回 BuildError
    Core::Expected<Core::Bytes> Build(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);

    /// 展开操作模板生成请求帧 (变长变量路径, P1 A)
    /// 标量变量查 variables; {Name:raw} 查 variableBytesHex (hex 字符串 → 字节流);
    /// 两表可独立使用, 也可并存 (同一 op 内不同占位符路由不同表)
    Core::Expected<Core::Bytes> BuildBytes(
        const Core::OperationConfig& op,
        const std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, std::string>& variableBytesHex,
        const std::unordered_map<std::string, std::string>& varAliasMap,
        AutoComputeProvider& autoProvider);

    // 协议级 static 变量 + 标签级 variables 合并
    // 返回一个新的 merged map: 协议级 static 变量为基, tag.variables 覆盖同名键
    // 调用方把返回值传给 Build/BuildBytes, 不修改原 map
    static std::unordered_map<std::string, uint32_t> MergeVariables(
        const std::unordered_map<std::string, uint32_t>& protocolDefaults,
        const std::unordered_map<std::string, uint32_t>& tagVariables);

    // 三段合并 (协议级 defaultVariables + op 级 static 覆盖 + 标签级覆盖).
    //   op.inputs 中仅 source=static 且有值的条目进入 ctx.variables (auto 走 AutoComputeProvider;
    //   无值 static 不进 ctx). 优先级: 标签 > op > 协议.
    static std::unordered_map<std::string, uint32_t> MergeVariables(
        const std::unordered_map<std::string, uint32_t>& protocolDefaults,
        const std::unordered_map<std::string, uint32_t>& opStaticVariables,
        const std::unordered_map<std::string, uint32_t>& tagVariables);

    // 从协议 inputs/outputs 段重建变量语义 (配置形态: 输入 static 在 inputs, 派生输出在 outputs;
    //   不存在平铺的单段 variables — 代际变迁见 ADR-0005).
    //   CollectStaticVariables  → inputs.source=static 条目 → ctx.variables 合并链的协议级基准.
    //   CollectAutoComputeJson  → inputs.source=auto 且非 derivedLength 条目 →
    //     AutoComputeProvider::DeclareJson 格式; outputs(derivedLength) 不进 (WriteBytes 阶段特判).
    static std::unordered_map<std::string, uint32_t> CollectStaticVariables(
        const Core::ProtocolConfig& protocol);
    static std::string CollectAutoComputeJson(const Core::ProtocolConfig& protocol);

    /// op 级 auto (非 derivedLength) 输入拼接进协议级 autoComputeJson。
    /// 单一实现 — Gateway(TagReader) 与 Service(FrameConsistencyCheck) 共用,
    /// 避免两侧语义漂移; derivedLength 不入此段 (参数层 InjectDerivedLengthVariables 注入)。
    static std::string MergeOpAutoComputeJson(const Core::ProtocolConfig& protocol,
                                              const Core::OperationConfig& op);

    // ── 模板布局与派生长度注入 (ADR-0012); 唯一实现在 Engine, Gateway 与 Service 共用 ──

    /// 扫描 requestTemplate 产出布局表 (宽度/首现偏移/固定段总宽).
    /// 与 RenderTemplate 的元素二分法一致: 整元素占位符 ({...}) 或 hex 字面量;
    /// 未知格式记 hasUnknown (宽度按 0). Gateway 与 Service 试算校验共用, 避免双实现漂移.
    static TemplateLayout BuildTemplateLayout(
        const std::vector<std::string>& requestTemplate);

    /// 派生长度变量注入 (参数层预解析): 扫描 protocolOutputs ∪ opOutputs 中
    /// source=auto strategy=derivedLength 声明, 按 expr 求值注入 variables
    /// (标签显式提供的同名变量优先 — 调用方须先放好显式值).
    /// totalBytes = 模板 {N:raw} 载荷实际字节总数; layout = BuildTemplateLayout 产物.
    /// expr 求值失败时该变量不注入 (渲染期将 BuildError "模板变量未提供").
    /// 返回 void: Engine 不承载任何协议族契约名 (如 PDULength) — 调用方也不需要
    ///   "是否含某协议字段"的结论, 不要再以返回值形式把协议约定引入本层.
    static void InjectDerivedLengthVariables(
        std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, Core::VariableConfig>& protocolOutputs,
        const std::unordered_map<std::string, Core::VariableConfig>& opOutputs,
        std::size_t totalBytes,
        const TemplateLayout& layout);
};

}} // namespace MyProt::Engine

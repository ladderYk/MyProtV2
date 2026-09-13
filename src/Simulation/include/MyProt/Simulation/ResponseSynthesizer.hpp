// src/Simulation/include/MyProt/Simulation/ResponseSynthesizer.hpp
// 应答合成器 — responseParser 的反向复用 (配置驱动仿真 L3 层)
//
// 客户端用 responseParser 校验应答; 仿真器把同一规格当作"应答布局说明书"
// 从请求反向生成应答帧 (Config_Schema §5):
//   validCondition "resp[N]==V" → 应答偏移 N 写字面量 V (如 FC 回显位)
//   dataLengthExpr  "resp[N]"   → 应答偏移 N 写实际数据长度
//   dataStartIndex M            → 应答前 M 字节镜像请求前缀, 数据区自 M 起
//   dataLengthExpr 为数字常量    → 数据区长度对齐该常量 (截断/补零)
// 最后按 framing 重算长度字段 — 与 LengthFieldFrameParser::CalculateTotalFrameSize
// 切帧公式互逆, 保证客户端能用同一份 framing 配置正确切出本帧。

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstddef>

#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Simulation {

class ResponseSynthesizer {
public:
    /// 从 OperationConfig 解析出的应答布局规格
    struct Spec {
        std::vector<std::pair<std::size_t, std::uint8_t> > asserts; ///< 偏移→字面量
        bool hasLenIndex;   ///< dataLengthExpr 形如 resp[N]
        std::size_t lenIndex;
        std::size_t constLen;     ///< dataLengthExpr 为数字常量 (0 = 无)
        std::size_t dataStartIndex;

        Spec() : hasLenIndex(false), lenIndex(0), constLen(0), dataStartIndex(0) {}
    };

    /// 解析操作的应答规格。文法不支持的子句静默忽略 (宽松解析)。
    static void ParseSpec(const Core::OperationConfig& op, Spec& out);

    /// 合成应答帧。
    /// @param request 已匹配的完整请求帧 (前缀镜像来源)
    /// @param data    应答数据区内容 (上层按变量从数据区取出)
    /// @param framing 协议帧配置 (长度字段重算依据)
    /// @return 应答帧; 无法合成时返回空
    static std::vector<std::uint8_t> Synthesize(const Spec& spec,
                                                const std::vector<std::uint8_t>& request,
                                                const std::vector<std::uint8_t>& data,
                                                const Core::FramingConfig& framing);

    /// 按自定义应答模板合成帧 (SimOperationConfig.responseTemplate, 非空时
    /// 覆盖 echo 反向合成)。文法:
    ///   hex 字面量  "AA 0B"      — 逐字节常量 (空格/制表符分隔)
    ///   {req:N:M}               — 从请求帧偏移 N 拷贝 M 字节 (回显)
    ///   {data}                  — 展开应答数据区 (read=寄存器值; write 为空)
    /// 拼接完成后按 framing 重算长度字段 (与 Synthesize 一致)。
    /// @return true 合成成功; false 文法错误或请求越界 (调用方不应答)
    static bool SynthesizeFromTemplate(
        const std::vector<std::string>& tmpl,
        const std::vector<std::uint8_t>& request,
        const std::vector<std::uint8_t>& data,
        const Core::FramingConfig& framing,
        std::vector<std::uint8_t>& out);

private:
    /// 按 LengthFieldConfig 重算并写入帧内长度字段
    static void ApplyLengthField(std::vector<std::uint8_t>& frame,
                                 const Core::LengthFieldConfig& lf);
};

}} // namespace MyProt::Simulation

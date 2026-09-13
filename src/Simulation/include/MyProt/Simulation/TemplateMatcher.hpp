// src/Simulation/include/MyProt/Simulation/TemplateMatcher.hpp
// 请求模板匹配器 — requestTemplate 的反向复用 (配置驱动仿真 L2 层)
//
// 客户端用 requestTemplate 构造请求; 仿真器把同一模板编译为"字节形状模式",
// 对收到的完整请求帧做反向识别: 得到操作名 + 从占位符位置提取变量值。
//
// 文法与 RequestBuilder 一致 (Config_Schema §3.2 实际实现子集):
//   - 十六进制字面量行: 偶数长度 hex 字符串, 每 2 字符 1 字节 (忽略空格)
//   - 占位符 {Name:Xn}      → 定宽变量段, 匹配时提取大端数值
//   - 占位符 {Name:auto:Xn} → 定宽通配段 (自增字段如 TransactionId), 跳过内容
//   Xn: hex 字符宽度, 偶数 2..16 → n/2 字节。
//   其余格式 (校验和函数占位符等) 编译期跳过该操作 — 与构建器未实现的文法保持一致。

#pragma once

#include <string>
#include <map>
#include <vector>
#include <cstddef>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Simulation {

/// 匹配结果
struct TemplateMatch {
    bool matched;
    std::string operation;                      ///< 命中的操作名
    std::map<std::string, std::uint32_t> variables; ///< 从占位符位置提取的变量
    std::size_t frameLength;                    ///< 期望帧长 (字节)

    TemplateMatch() : matched(false), frameLength(0) {}
};

class TemplateMatcher {
public:
    /// protocol 须在生命周期内保持有效 (持有其引用)
    explicit TemplateMatcher(const Core::ProtocolConfig& protocol);

    /// 编译全部操作模板; 协议内容变更后需重新调用
    void Compile();

    /// 对完整请求帧做匹配; 多操作命中时按协议声明序返回首个。
    /// frame 须是已切帧的完整请求 (TCP 由 FrameParser 切帧)。
    TemplateMatch Match(const Core::ByteView& frame) const;

    /// 编译期歧义报告 (上次 Compile() 的结果):
    /// 总长相同且逐字节约束兼容的操作对 — 存在可同时命中两者的帧,
    /// Match 只返回编译序首个, 实际路由可能违背配置意图 (仅告警不阻断)
    const std::vector<std::string>& Ambiguities() const { return _ambiguities; }

private:
    struct Segment {
        enum Kind { Literal, Variable, Wildcard } kind;
        std::vector<std::uint8_t> literal; ///< Literal: 期望字节序列
        std::string varName;               ///< Variable: 变量名
        int widthBytes;                    ///< Variable/Wildcard: 定宽字节数
        Segment() : kind(Literal), widthBytes(0) {}
    };
    struct CompiledOp {
        std::string name;
        std::vector<Segment> segments;
        std::size_t totalSize; ///< 模式总字节长 (帧长必须精确相等)
        CompiledOp() : totalSize(0) {}
    };

    static bool CompileLine(const std::string& raw, std::vector<Segment>& segs,
                            std::size_t& lineSize);
    static std::size_t WidthSpecToBytes(const std::string& widthSpec);

    const Core::ProtocolConfig& _protocol;
    std::vector<CompiledOp> _ops;
    std::vector<std::string> _ambiguities;
};

}} // namespace MyProt::Simulation

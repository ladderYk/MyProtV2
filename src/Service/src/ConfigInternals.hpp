// src/Service/src/ConfigInternals.hpp
// Service 内部契约：JSON→POCO 解析器与文法工具（不对外安装，仅 Service 库内共享）。
// 拆分前这些符号散落在 ConfigDeepValidator.cpp 的匿名命名空间，Schema/Semantic 检查
// 与 JSON 文本入口无法独立编译；现由 ProtocolJsonParser.cpp 实现、其余 TU 按需引用。

#pragma once

#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Service { namespace detail {

// ── 基础字符判定（文法工具共用） ──
bool IsHexChar(char c);
bool IsIdentStart(char c);
bool IsIdentChar(char c);

// ── 值语义判定 ──
/// {Name:raw} 变长字节注入标记（与 {Name:Xn} 定宽十六进制并列）
bool IsRawSpec(const std::string& s);

/// 协议 inputs(static 有值 / auto) 能否在运行时供给该模板变量
bool ProtocolResolvesInputVariable(const Core::ProtocolConfig& proto,
                                   const std::string& name);

// ── 模板布局工具（长度偏移自检共用） ──
/// 模板元素字节宽度：hex 字面量 token 数；{Name:Xn} → n/2；{Name:raw} → 0
int TemplateElementBytes(const std::string& el);
/// 提取 {Name...} 的变量名
bool TemplateElementName(const std::string& el, std::string& name);
/// 把 "{payload:len} ± C" 归约为常量偏移 C（其它形式返回 false）
bool TryPayloadOffset(const std::string& expr, const std::string& payloadVarName,
                      long& off);
/// 模板中全部 {Name:raw} 的变量名集合
std::set<std::string> CollectRawPlaceholderNames(
    const std::vector<std::string>& requestTemplate);

// ── simulation 段（协议级多实例 + server.json 旧单实例共用同一 schema） ──
/// simulation 对象 → POCO；类型/取值范围错误经 err 返回。
bool ParseSimulationDocImpl(const nlohmann::json& doc,
                            Core::SimulationConfig& sim, std::string& err);
/// 仿真应答模板单行文法：hex 字面量 | {data} | {req:N:M}（含空行）。
bool IsValidSimTemplateLine(const std::string& raw);
/// simulation 语义校验（端口/寄存器数/初值/操作），ctx 为报错前缀（如协议名）。
/// 返回全部错误，空 = 通过。
std::vector<std::string> ValidateSimulationConfig(
    const Core::SimulationConfig& sim, const std::string& ctx);

// ── JSON 文档 → POCO（结构错误经 err 返回；字段取值规则留给校验器） ──
/// 协议文档（protocols/*.json）；defaultVer 用于缺失 schemaVersion 的回填
bool ParseProtocolDocImpl(const nlohmann::json& doc, int defaultVer,
                          Core::ProtocolConfig& p, std::string& err);
/// 配置根文档（tags.json：devices/tags/resilience/webApi）
bool ParseRootDocImpl(const nlohmann::json& doc, int defaultVer,
                      Core::ConfigRoot& root, std::string& err);

}}} // namespace MyProt::Service::detail

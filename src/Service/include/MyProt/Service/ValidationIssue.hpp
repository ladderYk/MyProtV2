// src/Service/include/MyProt/Service/ValidationIssue.hpp
// 校验结果结构化 — 把 ValidationResult 的字符串条目映射为可机读的「问题」对象,
// 供 /api/validate 只读端点与管理面 (webui) 做分类渲染 / 点击定位。
//
// 为什么在边界做分类, 而不是改写校验器:
//   ConfigDeepValidator 有 ~60 处 r.addError("..."), 逐处改成结构体属于大改且没有回归网;
//   而所有消息都是稳定格式 ("操作 X ..." / "设备 X ..." / "标签 X ..."), 用一张规则表做
//   子串匹配即可拿到 (ruleId, subject, 建议定位路径)。
//   关键约束: **只增信息, 不丢信息** — 未命中规则表的条目仍原样返回
//   (ruleId="unclassified"), message 始终是校验器原文。
//
// 分层: 本头文件属于 Service 层, 只依赖 <string>/<vector> (不引入 nlohmann);
//   JSON 序列化由 WebApi/App 层完成 (见 src/App/ValidateApi.cpp)。
#pragma once

#include <string>
#include <vector>

namespace MyProt { namespace Service {

/// 单条校验问题 — severity=error 阻断保存; warning 仅提示 (原 "[WARN] " 前缀条目)
struct ValidationIssue {
    std::string severity;   // "error" | "warning"
    std::string ruleId;     // 规则标识, 如 "frame.length_consistency"; 未归类 = "unclassified"
    std::string scope;      // 校验域: "protocol" | "tags" (调用方传入)
    std::string subject;    // 定位主体: 操作名 / 设备 ID / 标签名 (可为空)
    std::string field;      // 建议定位路径, 如 "operations.WriteRegisters.requestTemplate" (可为空)
    std::string message;    // 校验器原始消息 (已剥离 "[WARN] " 前缀)
};

/// 把 ConfigStore::Validate 的条目列表分类为结构化问题。
/// - severity: 以 "[WARN] " 前缀判定
/// - ruleId/subject/field: 按内部规则表匹配; 未命中 → "unclassified" + 空 subject/field
/// - 顺序与入参一致, 条目数不减少 (不丢弃任何一条)
std::vector<ValidationIssue> ClassifyValidationItems(
    const std::vector<std::string>& items, const std::string& scope);

}} // namespace MyProt::Service

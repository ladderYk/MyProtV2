// src/Service/src/ConfigJsonText.hpp
// 配置文件 JSON 解析策略 (单一入口) — 允许 // 行注释与 /* */ 块注释,
// 并容忍 UTF-8 BOM。HTTP 请求体不走这里: 客户端生成的报文应保持严格 JSON。
//
// 为什么允许注释: 协议文件是人工手写的字节布局, 注释能把 "这个常量来自手册哪一节"
// 写在字段旁边; 注释在解析前由 nlohmann 的 ignore_comments 丢弃, 不进入 POCO,
// 因此对校验/渲染/热重载没有任何语义影响。
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace MyProt { namespace Service { namespace detail {

/// Parse config-file JSON text. Throws nlohmann::json::parse_error on failure;
/// callers keep their existing try/catch so the error text stays unchanged.
inline nlohmann::json ParseConfigJsonText(const std::string& text) {
    // UTF-8 BOM: Windows 编辑器另存为 "UTF-8 with BOM" 时首三字节会破坏 JSON 解析
    const std::size_t kSkip =
        (text.size() >= 3
         && static_cast<unsigned char>(text[0]) == 0xEF
         && static_cast<unsigned char>(text[1]) == 0xBB
         && static_cast<unsigned char>(text[2]) == 0xBF) ? 3 : 0;
    const std::string body = kSkip ? text.substr(kSkip) : text;
    return nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/true,
                                 /*ignore_comments=*/true);
}

}}} // namespace MyProt::Service::detail

// src/WebApi/include/MyProt/WebApi/QueryString.hpp — HTTP query 参数解析 (header-only)
// 管理面自实现最小 HTTP/1.1, 没有框架代劳解码; 这里统一提供:
//   - QueryParam(path, key): 按 & 分段、按 = 取第一个值, 未命中返回空串
//   - UrlDecode(s): 仅解 %XX; 非法/截断序列原样保留
// 刻意不把 '+' 当空格: token 等参数可能含裸 '+', 需要空格请用 %20。
#pragma once

#include <cstddef>
#include <string>

namespace MyProt { namespace WebApi {

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// 百分号解码: "%E6%B5%8B" → "测"; "%2" / "%ZZ" 等非法序列原样保留
inline std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = HexDigit(s[i + 1]);
            const int lo = HexDigit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

/// 从 "/path?key=v&..." 提取 query 参数值 (结果已解码; 未找到返回空串)
inline std::string QueryParam(const std::string& path, const std::string& key) {
    const std::size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    std::size_t pos = q + 1;
    while (pos < path.size()) {
        const std::size_t amp = path.find('&', pos);
        const std::string kv = path.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return UrlDecode(kv.substr(eq + 1));
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

}} // namespace MyProt::WebApi

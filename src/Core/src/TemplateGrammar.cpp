// src/Core/src/TemplateGrammar.cpp
// 请求模板文法的唯一实现 — Engine / Simulation / Service 三方共用。
// 解析结果 (Segment) 由各消费方自行解释: 渲染、反向匹配或加载期校验。

#include "MyProt/Core/TemplateGrammar.hpp"

#include <cctype>

namespace MyProt { namespace Core { namespace TemplateGrammar {

namespace {

bool IsIdentStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool IsIdentChar(char c) {
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool HasLittleEndianSuffix(const std::string& spec) {
    return spec.size() > 2
        && spec.compare(spec.size() - 2, 2, "LE") == 0;
}

void SetError(std::string* error, const char* text) {
    if (error) *error = text;
}

/// 去掉所有空白后的 hex 字符数; 用于区分"奇数位"与"非法字符"两类错误。
std::size_t CompactLength(const std::string& text) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(text[i]))) ++n;
    }
    return n;
}

} // namespace

int WidthSpecBytes(const std::string& formatSpec) {
    std::string core = formatSpec;
    if (HasLittleEndianSuffix(core)) core.resize(core.size() - 2);
    if (core.size() < 2 || core[0] != 'X') return 0;

    int hexWidth = 0;
    for (std::size_t i = 1; i < core.size(); ++i) {
        if (core[i] < '0' || core[i] > '9') return 0;
        hexWidth = hexWidth * 10 + (core[i] - '0');
    }
    if (hexWidth < 2 || hexWidth > 16 || hexWidth % 2 != 0) return 0;
    return hexWidth / 2;
}

std::vector<std::string> SplitSegments(const std::string& inner) {
    std::vector<std::string> segs;
    std::size_t pos = 0;
    while (true) {
        const std::size_t colon = inner.find(':', pos);
        segs.push_back(inner.substr(pos, colon - pos));
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return segs;
}

bool ParseHexLiteral(const std::string& text, std::vector<std::uint8_t>& out) {
    std::string compact;
    compact.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (!std::isspace(static_cast<unsigned char>(c))) compact += c;
    }
    if (compact.empty()) return true;                 // 空串 = 0 字节 (合法)
    if (compact.size() % 2 != 0) return false;
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        const int hi = HexNibble(compact[i]);
        const int lo = HexNibble(compact[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return true;
}

ParseStatus ParseElement(const std::string& token,
                         Segment& out,
                         bool strictIdentifier,
                         bool allowLegacyAuto,
                         std::string* error) {
    out = Segment();
    if (token.size() >= 2 && token[0] == '{' && token[token.size() - 1] == '}') {
        const std::string inner = token.substr(1, token.size() - 2);
        const std::vector<std::string> parts = SplitSegments(inner);

        if (parts.empty() || parts[0].empty()) {
            SetError(error, "模板段变量名为空");
            return ParseInvalid;
        }
        if (strictIdentifier) {
            if (!IsIdentStart(parts[0][0])) {
                SetError(error, "模板段变量名须以字母或下划线开头");
                return ParseInvalid;
            }
            for (std::size_t i = 1; i < parts[0].size(); ++i) {
                if (!IsIdentChar(parts[0][i])) {
                    SetError(error, "模板段变量名只能包含字母/数字/下划线");
                    return ParseInvalid;
                }
            }
        }

        // {Name:raw} — 变长字节注入
        if (parts.size() == 2 && parts[1] == "raw") {
            out.kind = kRaw;
            out.name = parts[0];
            return ParseOk;
        }

        // {Name:Xn} / {Name:XnLE}
        if (parts.size() == 2) {
            const int width = WidthSpecBytes(parts[1]);
            if (width <= 0) {
                SetError(error, "宽度说明须为 Xn 或 XnLE (偶数 hex 位数 2..16)");
                return ParseInvalid;
            }
            out.kind = kVariable;
            out.name = parts[0];
            out.widthBytes = width;
            out.littleEndian = HasLittleEndianSuffix(parts[1]);
            return ParseOk;
        }

        // {Name:auto:Xn} — 旧版三段文法, 仅仿真反向匹配接受为通配段
        if (parts.size() == 3 && parts[1] == "auto") {
            const int width = WidthSpecBytes(parts[2]);
            if (width <= 0) {
                SetError(error, "宽度说明须为 Xn 或 XnLE (偶数 hex 位数 2..16)");
                return ParseInvalid;
            }
            if (!allowLegacyAuto) {
                SetError(error, "模板段格式无效 (仅 {Name:Xn} / {Name:XnLE} / {Name:raw})");
                return ParseLegacyAuto;
            }
            out.kind = kWildcard;
            out.name = parts[0];
            out.widthBytes = width;
            out.littleEndian = HasLittleEndianSuffix(parts[2]);
            return ParseOk;
        }

        SetError(error, "模板段格式无效 (仅 {Name:Xn} / {Name:XnLE} / {Name:raw})");
        return ParseInvalid;
    }

    // 十六进制字面量
    if (!ParseHexLiteral(token, out.literal)) {
        SetError(error, CompactLength(token) % 2 != 0
            ? "十六进制字面量长度须为偶数"
            : "非法十六进制字符");
        return ParseInvalid;
    }
    out.kind = kLiteral;
    return ParseOk;
}

bool ParseLine(const std::string& raw,
               std::vector<Segment>& segments,
               std::size_t& lineSize,
               bool allowLegacyAuto,
               std::string* error) {
    const std::string line = Trim(raw);
    if (line.empty()) return true;

    Segment seg;
    const ParseStatus st = ParseElement(line, seg, false, allowLegacyAuto, error);
    if (st != ParseOk) return false;

    switch (seg.kind) {
        case kLiteral:
            lineSize += seg.literal.size();
            break;
        case kVariable:
        case kWildcard:
            lineSize += static_cast<std::size_t>(seg.widthBytes);
            break;
        case kRaw:
        default:
            break;   // raw 不贡献定长
    }
    segments.push_back(seg);
    return true;
}

bool SplitPlaceholder(const std::string& token, std::vector<std::string>& parts) {
    parts.clear();
    if (token.size() < 2 || token[0] != '{' || token[token.size() - 1] != '}') return false;

    const std::string inner = token.substr(1, token.size() - 2);
    std::string cur;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const char c = inner[i];
        if (c == ':') {
            parts.push_back(cur);
            cur.clear();
        } else if (IsIdentChar(c)) {
            cur += c;
        } else {
            return false;                              // 变量名/格式段只允许标识字符
        }
    }
    parts.push_back(cur);

    if (parts.empty() || parts[0].empty()) return false;
    if (!IsIdentStart(parts[0][0])) return false;
    return parts.size() <= 3;
}

bool IsValidPlaceholder(const std::string& token) {
    std::vector<std::string> parts;
    if (!SplitPlaceholder(token, parts)) return false;
    if (parts.size() != 2) return false;               // 三段 auto 文法已移除
    return WidthSpecBytes(parts[1]) > 0 || parts[1] == "raw";
}

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

}}} // namespace MyProt::Core::TemplateGrammar

// src/Simulation/src/ResponseSynthesizer.cpp
// 应答合成器实现 — 见 ResponseSynthesizer.hpp 文件头注释

#include "MyProt/Simulation/ResponseSynthesizer.hpp"
#include "MyProt/Core/Hex.hpp"

#include <cstdlib>
#include <cctype>
#include <algorithm>

namespace MyProt { namespace Simulation {

using Core::HexVal;   // 单一实现: MyProt/Core/Hex.hpp

namespace {

std::string Trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// 解析 "resp[N]" → N; 不匹配返回 false
bool ParseRespIndex(const std::string& expr, std::size_t& index) {
    const std::string s = Trim(expr);
    if (s.size() < 6) return false;
    if (s.compare(0, 4, "resp") != 0 || s[4] != '[' || s[s.size() - 1] != ']') {
        return false;
    }
    const std::string num = Trim(s.substr(5, s.size() - 6));
    if (num.empty()) return false;
    for (std::size_t i = 0; i < num.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(num[i]))) return false;
    }
    index = static_cast<std::size_t>(std::atoi(num.c_str()));
    return true;
}

/// 解析字面量数值: 支持 0x 前缀与十进制
bool ParseValue(const std::string& expr, std::uint32_t& value) {
    const std::string s = Trim(expr);
    if (s.empty()) return false;
    char* end = 0;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0); // base=0: 自动识别 0x
    if (end == s.c_str() || *end != '\0') return false;
    value = static_cast<std::uint32_t>(v);
    return true;
}

} // namespace

void ResponseSynthesizer::ParseSpec(const Core::OperationConfig& op, Spec& out) {
    out = Spec();

    int startIdx = op.responseParser.dataStartIndex;
    out.dataStartIndex = startIdx > 0 ? static_cast<std::size_t>(startIdx) : 0;

    // validCondition: "resp[N]==V" 子句, "&&" 连接多条
    const std::string& vc = op.responseParser.validCondition;
    std::size_t pos = 0;
    while (pos < vc.size()) {
        const std::size_t amp = vc.find("&&", pos);
        const std::string cond =
            Trim(vc.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos));
        pos = (amp == std::string::npos) ? vc.size() : amp + 2;

        // 单条: resp[L]==R
        const std::size_t eq = cond.find("==");
        if (eq == std::string::npos) continue;
        std::size_t lhsIdx = 0;
        std::uint32_t rhsVal = 0;
        if (!ParseRespIndex(cond.substr(0, eq), lhsIdx)) continue;
        if (!ParseValue(cond.substr(eq + 2), rhsVal)) continue;
        if (rhsVal > 0xFF) continue;
        out.asserts.push_back(std::make_pair(lhsIdx, static_cast<std::uint8_t>(rhsVal)));
    }

    // dataLengthExpr: "resp[N]" 或 数字常量
    const std::string de = Trim(op.responseParser.dataLengthExpr);
    if (!de.empty()) {
        std::size_t idx = 0;
        if (ParseRespIndex(de, idx)) {
            out.hasLenIndex = true;
            out.lenIndex = idx;
        } else {
            std::uint32_t v = 0;
            if (ParseValue(de, v)) out.constLen = static_cast<std::size_t>(v);
        }
    }
}

std::vector<std::uint8_t> ResponseSynthesizer::Synthesize(
    const Spec& spec,
    const std::vector<std::uint8_t>& request,
    const std::vector<std::uint8_t>& data,
    const Core::FramingConfig& framing) {

    // 1. 前缀镜像: 应答 [0, dataStartIndex) 复制请求前缀
    std::vector<std::uint8_t> frame;
    const std::size_t echoLen = std::min(request.size(), spec.dataStartIndex);
    frame.assign(request.begin(), request.begin() + static_cast<std::ptrdiff_t>(echoLen));
    if (frame.size() < spec.dataStartIndex) {
        frame.resize(spec.dataStartIndex, 0);
    }

    // 2. 数据区对齐: 常量长度优先 (客户端按常量解析), 否则用实际数据长
    std::size_t effectiveLen = data.size();
    if (spec.constLen > 0 && spec.constLen != data.size()) {
        effectiveLen = spec.constLen;
    }
    frame.insert(frame.end(), data.begin(),
                 data.begin() + static_cast<std::ptrdiff_t>(
                     std::min(data.size(), effectiveLen)));
    if (frame.size() < spec.dataStartIndex + effectiveLen) {
        frame.resize(spec.dataStartIndex + effectiveLen, 0);
    }

    // 3. 长度锚点写入 (客户端 dataLengthExpr 的解析位置)
    if (spec.hasLenIndex && spec.lenIndex < frame.size() && effectiveLen <= 0xFF) {
        frame[spec.lenIndex] = static_cast<std::uint8_t>(effectiveLen);
    }

    // 4. validCondition 断言覆写 (如 FC 回显位)
    for (std::size_t i = 0; i < spec.asserts.size(); ++i) {
        if (spec.asserts[i].first < frame.size()) {
            frame[spec.asserts[i].first] = spec.asserts[i].second;
        }
    }

    // 5. framing 长度字段重算 (与客户端切帧公式互逆)
    if (framing.type == Core::FramingType::LengthField) {
        ApplyLengthField(frame, framing.lengthField);
    }

    return frame;
}

bool ResponseSynthesizer::SynthesizeFromTemplate(
    const std::vector<std::string>& tmpl,
    const std::vector<std::uint8_t>& request,
    const std::vector<std::uint8_t>& data,
    const Core::FramingConfig& framing,
    std::vector<std::uint8_t>& out) {

    out.clear();

    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const std::string line = Trim(tmpl[i]);
        if (line.empty()) continue;

        // 占位符片段
        if (line[0] == '{') {
            if (line.size() < 2 || line[line.size() - 1] != '}') return false;
            const std::string inner = Trim(line.substr(1, line.size() - 2));

            if (inner == "data") {
                out.insert(out.end(), data.begin(), data.end());
                continue;
            }

            // {req:N:M}: N/M 十进制非负整数, N+M 不得越过请求帧尾
            if (inner.compare(0, 4, "req:") != 0) return false;
            const std::string range = inner.substr(4);
            const std::size_t colon = range.find(':');
            if (colon == std::string::npos) return false;
            const std::string nStr = Trim(range.substr(0, colon));
            const std::string mStr = Trim(range.substr(colon + 1));
            if (nStr.empty() || mStr.empty()) return false;
            bool digits = true;
            for (std::size_t k = 0; k < nStr.size(); ++k) {
                if (!std::isdigit(static_cast<unsigned char>(nStr[k]))) digits = false;
            }
            for (std::size_t k = 0; k < mStr.size(); ++k) {
                if (!std::isdigit(static_cast<unsigned char>(mStr[k]))) digits = false;
            }
            if (!digits) return false;
            const std::size_t off = static_cast<std::size_t>(std::atoi(nStr.c_str()));
            const std::size_t len = static_cast<std::size_t>(std::atoi(mStr.c_str()));
            if (off > request.size() || len > request.size() - off) return false;
            out.insert(out.end(),
                       request.begin() + static_cast<std::ptrdiff_t>(off),
                       request.begin() + static_cast<std::ptrdiff_t>(off + len));
            continue;
        }

        // hex 字面量: 空格/制表符分隔, 每 token 恰两位
        std::size_t tokStart = std::string::npos;
        for (std::size_t p = 0; p <= line.size(); ++p) {
            const bool sep = (p == line.size())
                || line[p] == ' ' || line[p] == '\t';
            if (sep) {
                if (tokStart != std::string::npos) {
                    if (p - tokStart != 2) return false;
                    const int hi = HexVal(line[tokStart]);
                    const int lo = HexVal(line[tokStart + 1]);
                    out.push_back(static_cast<std::uint8_t>(hi * 16 + lo));
                    tokStart = std::string::npos;
                }
                continue;
            }
            if (HexVal(line[p]) < 0) return false;
            if (tokStart == std::string::npos) tokStart = p;
        }
    }

    if (out.empty()) return false;

    // 长度字段重算 — 与 echo 合成保持同一契约
    if (framing.type == Core::FramingType::LengthField) {
        ApplyLengthField(out, framing.lengthField);
    }
    return true;
}

void ResponseSynthesizer::ApplyLengthField(std::vector<std::uint8_t>& frame,
                                           const Core::LengthFieldConfig& lf) {
    const std::size_t total = frame.size();
    const std::size_t off = static_cast<std::size_t>(lf.lengthFieldOffset);

    // CalculateTotalFrameSize 的逆运算:
    //   includesHeader: total = value + adjustment      → value = total - adjustment
    //   else:           total = header + value + adj   → value = total - header - adj
    std::size_t value = total;
    if (lf.lengthIncludesHeader) {
        if (total < lf.lengthAdjustment) return;
        value = total - lf.lengthAdjustment;
    } else {
        if (total < lf.headerLength + lf.lengthAdjustment) return;
        value = total - lf.headerLength - lf.lengthAdjustment;
    }

    switch (lf.lengthFieldLength) {
        case 1:
            if (off < total) frame[off] = static_cast<std::uint8_t>(value & 0xFF);
            break;
        case 2:
            if (off + 1 < total) {
                if (lf.byteOrder == Core::ByteOrder::BigEndian) {
                    frame[off]     = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                    frame[off + 1] = static_cast<std::uint8_t>(value & 0xFF);
                } else {
                    frame[off]     = static_cast<std::uint8_t>(value & 0xFF);
                    frame[off + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                }
            }
            break;
        case 4:
            if (off + 3 < total) {
                for (int b = 0; b < 4; ++b) {
                    const int shift = lf.byteOrder == Core::ByteOrder::BigEndian
                                          ? (3 - b) * 8 : b * 8;
                    frame[off + static_cast<std::size_t>(b)] =
                        static_cast<std::uint8_t>((value >> shift) & 0xFF);
                }
            }
            break;
        default:
            break;
    }
}

}} // namespace MyProt::Simulation

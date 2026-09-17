// src/Simulation/src/TemplateMatcher.cpp
// 请求模板匹配器实现 — 见 TemplateMatcher.hpp 文件头注释

#include "MyProt/Simulation/TemplateMatcher.hpp"
#include "MyProt/Core/Hex.hpp"

#include <cstdlib>
#include <cstring>
#include <cctype>

namespace MyProt { namespace Simulation {

using Core::HexVal;   // 单一实现: MyProt/Core/Hex.hpp

namespace {

std::string StripSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(s[i]))) out.push_back(s[i]);
    }
    return out;
}

/// 按 ':' 切分占位符内部段 (与 RequestBuilder::SplitSegments 一致, 空段丢弃)
std::vector<std::string> SplitSegments(const std::string& inner) {
    std::vector<std::string> segs;
    std::string cur;
    for (std::size_t i = 0; i <= inner.size(); ++i) {
        if (i == inner.size() || inner[i] == ':') {
            if (!cur.empty()) segs.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(inner[i]);
        }
    }
    return segs;
}

} // namespace

TemplateMatcher::TemplateMatcher(const Core::ProtocolConfig& protocol)
    : _protocol(protocol) {
}

std::size_t TemplateMatcher::WidthSpecToBytes(const std::string& widthSpec) {
    // Xn / XnLE → n/2 字节 (与引擎 RenderTemplate 同一文法)。
    // LE 后缀不改变匹配/提取语义: 字节仍按大端装配出值, 小端字段由业务侧
    // 按 dataByteOrder 解释 — 匹配只关心形状与定宽。
    std::string core = widthSpec;
    if (core.size() > 2 && core.compare(core.size() - 2, 2, "LE") == 0) {
        core.resize(core.size() - 2);
    }
    if (core.size() < 2 || core[0] != 'X') return 0;
    int hexWidth = 0;
    for (size_t i = 1; i < core.size(); ++i) {
        if (core[i] < '0' || core[i] > '9') return 0;
        hexWidth = hexWidth * 10 + (core[i] - '0');
    }
    if (hexWidth < 2 || hexWidth > 16 || hexWidth % 2 != 0) return 0;
    return static_cast<std::size_t>(hexWidth / 2);
}

bool TemplateMatcher::CompileLine(const std::string& raw,
                                  std::vector<Segment>& segs,
                                  std::size_t& lineSize) {
    const std::string line = StripSpaces(raw);
    if (line.empty()) return true;

    if (line.size() >= 2 && line[0] == '{' && line[line.size() - 1] == '}') {
        const std::string inner = line.substr(1, line.size() - 2);
        const std::vector<std::string> sgs = SplitSegments(inner);
        if (sgs.empty()) return false;

        // {Name:Xn} / {Name:auto:Xn} / {Name:raw}; 其余文法不支持 → 该行编译失败
        if (sgs.size() != 2 && !(sgs.size() == 3 && sgs[1] == "auto")) return false;

        // {Name:raw} 变长尾段: 仅允许作为模板最后一行 (Compile 侧校验末段
        // 位置; raw 之后的定长段偏移不可知, 非末位 raw 编译失败)
        if (sgs.size() == 2 && sgs[1] == "raw") {
            Segment seg;
            seg.kind = Segment::Raw;
            seg.varName = sgs[0];
            segs.push_back(seg);
            return true;
        }

        Segment seg;
        seg.widthBytes = static_cast<int>(WidthSpecToBytes(sgs[sgs.size() - 1]));
        if (seg.widthBytes <= 0) return false;

        if (sgs.size() == 3) {
            seg.kind = Segment::Wildcard; // auto 自增字段: 定宽通配
        } else {
            seg.kind = Segment::Variable;
            seg.varName = sgs[0];
        }
        lineSize += static_cast<std::size_t>(seg.widthBytes);
        segs.push_back(seg);
        return true;
    }

    // 十六进制字面量行
    if (line.size() % 2 != 0) return false;
    Segment seg;
    seg.literal.reserve(line.size() / 2);
    for (std::size_t c = 0; c + 1 < line.size(); c += 2) {
        const int hi = HexVal(line[c]);
        const int lo = HexVal(line[c + 1]);
        if (hi < 0 || lo < 0) return false;
        seg.literal.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    lineSize += seg.literal.size();
    segs.push_back(seg);
    return true;
}

void TemplateMatcher::Compile() {
    _ops.clear();
    _ambiguities.clear();

    for (const auto& kv : _protocol.operations) {
        CompiledOp op;
        op.name = kv.first;

        bool ok = true;
        for (std::size_t i = 0; i < kv.second.requestTemplate.size() && ok; ++i) {
            const std::size_t before = op.totalSize;
            ok = CompileLine(kv.second.requestTemplate[i], op.segments, op.totalSize);
            if (!ok) op.totalSize = before; // 回滚半截行 (保持 totalSize 一致性)
        }
        if (!ok || op.totalSize == 0) continue; // 编译失败的操作不参与匹配

        // 变长尾段校验: {Name:raw} 仅允许作为末段 (CompileLine 已拒绝非首行
        // raw; 此处拒绝"raw 在首行但后随定长段"的非法形状)。合法时 totalSize
        // = 定长前缀长, 匹配按 "帧长 >= 前缀" 判定。
        bool rawMid = false;
        for (std::size_t s = 0; s + 1 < op.segments.size(); ++s) {
            if (op.segments[s].kind == Segment::Raw) { rawMid = true; break; }
        }
        if (rawMid) continue;
        if (!op.segments.empty()
                && op.segments[op.segments.size() - 1].kind == Segment::Raw) {
            op.hasTail = true;
        }

        _ops.push_back(op);
    }

    // ── 静态歧义检测 ──
    // 两模式可同时命中同一帧 ⟺ 总长相等且逐位置字节约束兼容:
    // 双 Literal 要求字节相等; 任一方为 Variable/Wildcard 即兼容。
    // (展开到字节级比较 — 两模式的段切分可以不同, 如 [Lit2][Var2] vs [Var4])
    if (_ops.size() < 2) return;

    std::vector<std::pair<bool, int> > a; // (isFixed, value) — 复用缓冲
    std::vector<std::pair<bool, int> > b;
    for (std::size_t i = 0; i < _ops.size(); ++i) {
        const CompiledOp& oi = _ops[i];
        a.clear();
        for (std::size_t s = 0; s < oi.segments.size(); ++s) {
            const Segment& seg = oi.segments[s];
            if (seg.kind == Segment::Literal) {
                for (std::size_t k = 0; k < seg.literal.size(); ++k) {
                    a.push_back(std::make_pair(true,
                        static_cast<int>(seg.literal[k])));
                }
            } else {
                for (int k = 0; k < seg.widthBytes; ++k) {
                    a.push_back(std::make_pair(false, 0));
                }
            }
        }

        for (std::size_t j = i + 1; j < _ops.size(); ++j) {
            const CompiledOp& oj = _ops[j];
            // 变长尾段模式无固定帧长, 不参与定长歧义比对
            if (oi.hasTail || oj.hasTail) continue;
            if (oj.totalSize != oi.totalSize) continue;
            b.clear();
            for (std::size_t s = 0; s < oj.segments.size(); ++s) {
                const Segment& seg = oj.segments[s];
                if (seg.kind == Segment::Literal) {
                    for (std::size_t k = 0; k < seg.literal.size(); ++k) {
                        b.push_back(std::make_pair(true,
                            static_cast<int>(seg.literal[k])));
                    }
                } else {
                    for (int k = 0; k < seg.widthBytes; ++k) {
                        b.push_back(std::make_pair(false, 0));
                    }
                }
            }

            bool compatible = true;
            for (std::size_t p = 0; p < a.size() && compatible; ++p) {
                if (a[p].first && b[p].first && a[p].second != b[p].second) {
                    compatible = false;
                }
            }
            if (!compatible) continue;
            _ambiguities.push_back(
                "操作 '" + oi.name + "' 与 '" + oj.name + "' 模板形状重叠 (" +
                std::to_string(oi.totalSize) +
                " 字节约束兼容): 同一帧可同时命中两者, 实际按匹配序取首个");
        }
    }
}

TemplateMatch TemplateMatcher::Match(const Core::ByteView& frame) const {
    TemplateMatch out;
    if (frame.data == 0 && frame.size != 0) return out;

    for (std::size_t i = 0; i < _ops.size(); ++i) {
        const CompiledOp& op = _ops[i];
        // 定长模式: 帧长须精确相等; 变长尾段模式: 帧长 >= 定长前缀
        if (op.hasTail) {
            if (frame.size < op.totalSize) continue;
        } else if (op.totalSize != frame.size) {
            continue;
        }

        TemplateMatch cand;
        cand.matched = true;
        cand.operation = op.name;
        cand.frameLength = frame.size;

        bool ok = true;
        std::size_t off = 0;
        for (std::size_t s = 0; s < op.segments.size() && ok; ++s) {
            const Segment& seg = op.segments[s];
            const std::uint8_t* p = frame.data + off;
            switch (seg.kind) {
                case Segment::Literal:
                    if (std::memcmp(p, &seg.literal[0], seg.literal.size()) != 0) {
                        ok = false;
                    } else {
                        off += seg.literal.size();
                    }
                    break;
                case Segment::Variable: {
                    std::uint32_t v = 0;
                    for (int b = 0; b < seg.widthBytes; ++b) {
                        v = (v << 8) | static_cast<std::uint32_t>(p[b]);
                    }
                    cand.variables[seg.varName] = v;
                    off += static_cast<std::size_t>(seg.widthBytes);
                    break;
                }
                case Segment::Wildcard:
                    off += static_cast<std::size_t>(seg.widthBytes);
                    break;
                case Segment::Raw:
                    // 尾段消耗剩余全部字节, 内容不捕获 (写数据提取走 dataOffset)
                    off = frame.size;
                    break;
                default:
                    ok = false;
                    break;
            }
        }

        if (ok && off == frame.size) return cand;
    }
    return out;
}

}} // namespace MyProt::Simulation

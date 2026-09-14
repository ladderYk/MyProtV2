// src/Engine/src/AutoComputeProvider.cpp
// :auto 占位符通用求值器 — 4 个内置策略 (autoIncrement / frameSlice / expr / crc)
//   PIMPL 模式: 头文件不引入 nlohmann, 内部用极简 JSON 解析 (autoCompute 段格式固定)
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Core/Config.hpp"   // kExprMagicFrameLen/End, kPropLength/Offset/Fixed
#include "MyProt/Engine/MiniExpression.hpp"
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace MyProt { namespace Engine {

// ── 极简 JSON 值类型 (本命名空间内, 仅在 cpp 内用, 头文件不暴露) ──
struct JVal {
    enum Kind { Null_, Bool_, Num_, Str_, Arr_, Obj_ };
    Kind kind = Null_;
    bool   b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;   // 保序

    const JVal* Find(const std::string& k) const {
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string Str(const std::string& k, const std::string& def = "") const {
        const JVal* p = Find(k);
        if (!p || p->kind != Str_) return def;
        return p->s;
    }
    bool Has(const std::string& k) const { return Find(k) != nullptr; }
};

namespace {

struct JParser {
    const std::string& src;
    size_t pos = 0;
    std::string err;
    explicit JParser(const std::string& x) : src(x) {}

    void SkipWs() {
        while (pos < src.size() && (src[pos] == ' ' || src[pos] == '\t' ||
               src[pos] == '\n' || src[pos] == '\r')) ++pos;
    }
    bool Eat(char c) {
        SkipWs();
        if (pos < src.size() && src[pos] == c) { ++pos; return true; }
        return false;
    }
    bool Peek(char c) { SkipWs(); return pos < src.size() && src[pos] == c; }

    JVal ParseValue() {
        SkipWs();
        if (pos >= src.size()) { err = "unexpected end"; return JVal(); }
        char c = src[pos];
        if (c == '{') return ParseObj();
        if (c == '[') return ParseArr();
        if (c == '"') return ParseStr();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == 'n') return ParseNull();
        return ParseNum();
    }
    JVal ParseObj() {
        JVal v; v.kind = JVal::Obj_;
        if (!Eat('{')) { err = "expected {"; return v; }
        SkipWs();
        if (Peek('}')) { ++pos; return v; }
        while (true) {
            SkipWs();
            auto key = ParseStr();
            if (!err.empty()) return v;
            if (!Eat(':')) { err = "expected :"; return v; }
            auto val = ParseValue();
            if (!err.empty()) return v;
            v.obj.push_back({key.s, val});
            SkipWs();
            if (Peek(',')) { ++pos; continue; }
            if (Eat('}')) break;
            err = "expected , or }"; return v;
        }
        return v;
    }
    JVal ParseArr() {
        JVal v; v.kind = JVal::Arr_;
        if (!Eat('[')) { err = "expected ["; return v; }
        SkipWs();
        if (Peek(']')) { ++pos; return v; }
        while (true) {
            v.arr.push_back(ParseValue());
            if (!err.empty()) return v;
            SkipWs();
            if (Peek(',')) { ++pos; continue; }
            if (Eat(']')) break;
            err = "expected , or ]"; return v;
        }
        return v;
    }
    JVal ParseStr() {
        JVal v; v.kind = JVal::Str_;
        if (!Eat('"')) { err = "expected \""; return v; }
        std::string out;
        while (pos < src.size() && src[pos] != '"') {
            char c = src[pos++];
            if (c == '\\' && pos < src.size()) {
                char e = src[pos++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        if (pos + 4 > src.size()) { err = "bad \\u"; return v; }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = src[pos++];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        }
                        if (cp < 0x80) {
                            out.push_back((char)cp);
                        } else if (cp < 0x800) {
                            out.push_back((char)(0xC0 | (cp >> 6)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back((char)(0xE0 | (cp >> 12)));
                            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back((char)(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: out.push_back(e); break;
                }
            } else {
                out.push_back(c);
            }
        }
        if (pos >= src.size()) { err = "unterminated string"; return v; }
        ++pos;
        v.s = std::move(out);
        return v;
    }
    JVal ParseNum() {
        JVal v; v.kind = JVal::Num_;
        size_t start = pos;
        if (pos < src.size() && src[pos] == '-') ++pos;
        while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9')) ++pos;
        if (pos < src.size() && src[pos] == '.') {
            ++pos;
            while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9')) ++pos;
        }
        if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
            ++pos;
            if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) ++pos;
            while (pos < src.size() && (src[pos] >= '0' && src[pos] <= '9')) ++pos;
        }
        std::string sub = src.substr(start, pos - start);
        v.n = std::strtod(sub.c_str(), nullptr);
        return v;
    }
    JVal ParseBool() {
        JVal v; v.kind = JVal::Bool_;
        if (src.compare(pos, 4, "true") == 0) { v.b = true; pos += 4; return v; }
        if (src.compare(pos, 5, "false") == 0) { v.b = false; pos += 5; return v; }
        err = "expected bool"; return v;
    }
    JVal ParseNull() {
        JVal v;
        if (src.compare(pos, 4, "null") == 0) { pos += 4; return v; }
        err = "expected null"; return v;
    }
};

// 把 JVal 提取成 uint64 (数字直接取, 字符串按 strtoull(str, nullptr, 0) 解析)
inline uint64_t JToU64(const JVal& v, uint64_t def = 0) {
    if (v.kind == JVal::Num_) return (uint64_t)v.n;
    if (v.kind == JVal::Str_) {
        try { return std::stoull(v.s, nullptr, 0); } catch (...) { return def; }
    }
    if (v.kind == JVal::Bool_) return v.b ? 1u : 0u;
    return def;
}

// ── 工具: 解析 "from"/"to" 段说明符 ──
struct SlicePos {
    enum Kind { Absolute, End, EndOffset };
    Kind kind = Absolute;
    int32_t value = 0;
    bool valid = false;
};

SlicePos ParseSlicePos(const std::string& s) {
    SlicePos r;
    if (s == "end") { r.kind = SlicePos::End; r.valid = true; return r; }
    if (s.size() > 4 && s.compare(0, 4, "end-") == 0) {
        r.kind = SlicePos::EndOffset;
        r.value = std::atoi(s.c_str() + 4);
        r.valid = true; return r;
    }
    if (s.size() > 4 && s.compare(0, 4, "end+") == 0) {
        r.kind = SlicePos::EndOffset;
        r.value = -std::atoi(s.c_str() + 4);
        r.valid = true; return r;
    }
    char* endp = nullptr;
    long v = std::strtol(s.c_str(), &endp, 0);
    if (endp != s.c_str() && endp != s.c_str() + (long)s.size() && v >= 0) {
        r.kind = SlicePos::Absolute;
        r.value = (int32_t)v;
        r.valid = true; return r;
    }
    return r;
}

int32_t ResolveSlicePos(const SlicePos& p, size_t frameLen) {
    switch (p.kind) {
        case SlicePos::Absolute: return p.value;
        case SlicePos::End: return (int32_t)frameLen;
        case SlicePos::EndOffset: return (int32_t)frameLen + p.value;
    }
    return 0;
}

uint64_t ReadSliceValue(const uint8_t* data, size_t len, int32_t start, int32_t end, const std::string& endian) {
    if (start < 0) start = 0;
    if (end > (int32_t)len) end = (int32_t)len;
    int32_t n = end - start;
    if (n <= 0) return 0;
    if (n > 8) n = 8;
    uint64_t v = 0;
    bool big = (endian == "big" || endian == "BE" || endian.empty());
    if (big) {
        for (int i = 0; i < n; ++i) v = (v << 8) | data[start + i];
    } else {
        for (int i = 0; i < n; ++i) v |= (uint64_t)data[start + i] << (i * 8);
    }
    return v;
}

// CRC 表 (多项式参数化, 256 项预计算)
struct CrcTable {
    uint32_t poly;
    uint32_t init;
    uint32_t xorOut;
    bool reflectIn;
    bool reflectOut;
    int width;
    std::vector<uint32_t> table;

    uint32_t Compute(const uint8_t* data, size_t len) const {
        uint64_t crc = init;
        uint64_t mask = (width == 32) ? 0xFFFFFFFFull : 0xFFFFull;
        for (size_t i = 0; i < len; ++i) {
            uint8_t b = data[i];
            if (reflectIn) {
                b = (uint8_t)((b * 0x0202020202ull & 0x010884422010ull) % 1023);
            }
            crc ^= ((uint64_t)b << (width - 8));
            for (int j = 0; j < 8; ++j) {
                if (crc & ((width == 32) ? 0x80000000ull : 0x8000ull)) {
                    crc = (crc << 1) ^ poly;
                } else {
                    crc = (crc << 1);
                }
            }
            crc &= mask;
        }
        if (reflectOut) {
            uint64_t r = 0;
            for (int i = 0; i < width; ++i) {
                if (crc & ((uint64_t)1 << i)) r |= ((uint64_t)1 << (width - 1 - i));
            }
            crc = r;
        }
        return (uint32_t)(crc ^ xorOut);
    }
};

// 只认三个受支持算法名 — 与 ConfigDeepValidator 的 params.algo 白名单一致.
//   不接受 "crc16-mbus" / "modbus" / "ccitt" 之类的未文档化别名:
//   协议名 (modbus) 不应作为 CRC 算法别名出现在 Engine 层.
CrcTable GetCrcTable(const std::string& algo) {
    CrcTable t{};
    if (algo == "crc16-modbus") {
        t.width = 16; t.poly = 0xA001; t.init = 0xFFFF; t.xorOut = 0x0000;
        t.reflectIn = true; t.reflectOut = true;
    } else if (algo == "crc16-ccitt") {
        t.width = 16; t.poly = 0x1021; t.init = 0xFFFF; t.xorOut = 0x0000;
        t.reflectIn = false; t.reflectOut = false;
    } else if (algo == "crc32") {
        t.width = 32; t.poly = 0xEDB88320; t.init = 0xFFFFFFFF; t.xorOut = 0xFFFFFFFF;
        t.reflectIn = true; t.reflectOut = true;
    } else {
        throw std::runtime_error("AutoCompute: 未知 CRC 算法: " + algo +
            " (支持: crc16-modbus / crc16-ccitt / crc32)");
    }
    t.table.resize(256);
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        if (t.reflectIn) {
            for (int j = 0; j < 8; ++j) c = (c & 1) ? ((c >> 1) ^ t.poly) : (c >> 1);
        } else {
            for (int j = 0; j < 8; ++j) {
                c = (c & (1u << (t.width - 1))) ? ((c << 1) ^ t.poly) : (c << 1);
            }
            c &= (t.width == 32) ? 0xFFFFFFFFu : 0xFFFFu;
        }
        t.table[i] = c;
    }
    return t;
}

} // namespace

// ── Impl: 内部实现 ──
// 线程模型: 三份可变状态各自受锁保护, 策略执行不持锁.
//   counters   — countersMutex (原子自增语义)
//   rules      — rulesMutex; 规则对象以 shared_ptr<const> 发布, 读者持锁取快照后免锁使用
//   exprCache  — exprCacheMutex (仅 find/insert 加锁; Parse/Evaluate 在锁外)
struct AutoComputeProvider::Impl {
    // 1. autoIncrement 计数器
    std::unordered_map<std::string, uint64_t> counters;
    std::mutex countersMutex;

    // 2. 变量名 → 策略声明 (写入一次, 之后只读)
    struct DeclRule {
        std::string strategy;       // "autoIncrement" | "frameSlice" | "expr" | "crc"
        JVal params;                // 策略参数 (rule.params 子对象)
    };
    std::unordered_map<std::string, std::shared_ptr<const DeclRule> > rules;
    std::mutex rulesMutex;
    /// 最近一次成功声明的原文 — 内容相同即跳过 JSON 解析与规则重建.
    ///   轮询热路径每请求都会 SyncAutoComputeRules (CollectAutoComputeJson →
    ///   DeclareJson), 无此快路径则每请求重复 序列化 + 解析 + 整表重建.
    std::string declaredJson;

    // 3. expr 缓存: 避免每次重新解析表达式
    std::unordered_map<std::string, std::shared_ptr<MiniExpression::Parser> > exprCache;
    std::mutex exprCacheMutex;
};

// ────────── AutoComputeProvider 实现 ──────────

AutoComputeProvider::AutoComputeProvider() : _impl(new Impl()) {}
AutoComputeProvider::~AutoComputeProvider() = default;

// ── 原子自增 (未声明策略时的兜底路径) ──
uint64_t AutoComputeProvider::Next(const std::string& name, int byteWidth) {
    if (byteWidth < 1) byteWidth = 1;
    if (byteWidth > 8) byteWidth = 8;
    uint64_t mod = (byteWidth == 8) ? 0 : (uint64_t(1) << (byteWidth * 8));
    std::lock_guard<std::mutex> lk(_impl->countersMutex);
    uint64_t& c = _impl->counters[name];
    c = (mod == 0) ? (c + 1) : ((c + 1) % mod);
    return c;
}
void AutoComputeProvider::Reset(const std::string& name) {
    std::lock_guard<std::mutex> lk(_impl->countersMutex);
    _impl->counters.erase(name);
}

// ── 加载 autoCompute 段 (字符串 JSON) ──
bool AutoComputeProvider::DeclareJson(const std::string& autoComputeJson) {
    if (autoComputeJson.empty()) return false;
    // 快路径: 内容与最近一次声明相同 → 规则表已是同构内容, 免解析免重建.
    //   稳态轮询下每次请求都会走到这里, 是本函数的主要收益点.
    {
        std::lock_guard<std::mutex> lk(_impl->rulesMutex);
        if (autoComputeJson == _impl->declaredJson) return true;
    }
    JParser jp(autoComputeJson);
    JVal j = jp.ParseValue();
    if (!jp.err.empty() || j.kind != JVal::Obj_) return false;
    // 整段重声明 = 替换全部规则 (计数器保留, Next 自增跨调用).
    // 解析在锁外完成, 仅在发布时短暂持锁 — 读者不会被解析耗时阻塞.
    std::unordered_map<std::string, std::shared_ptr<const Impl::DeclRule> > next;
    for (auto& kv : j.obj) {
        const std::string& name = kv.first;
        const JVal& rule = kv.second;
        if (rule.kind != JVal::Obj_) continue;
        const JVal* strat = rule.Find("strategy");
        if (!strat || strat->kind != JVal::Str_) continue;
        std::shared_ptr<Impl::DeclRule> dr(new Impl::DeclRule());
        dr->strategy = strat->s;
        // params 归一 — 仅取 rule.params 子对象 (无 params 键 = 空参数)
        const JVal* pms = rule.Find("params");
        if (pms && pms->kind == JVal::Obj_) {
            dr->params = *pms;
        }
        next[name] = std::shared_ptr<const Impl::DeclRule>(dr);
    }
    {
        std::lock_guard<std::mutex> lk(_impl->rulesMutex);
        _impl->rules.swap(next);
        _impl->declaredJson = autoComputeJson;
    }
    return true;
}

bool AutoComputeProvider::IsDeclared(const std::string& name) const {
    std::lock_guard<std::mutex> lk(_impl->rulesMutex);
    return _impl->rules.find(name) != _impl->rules.end();
}

bool AutoComputeProvider::IsAutoIncrement(const std::string& name) const {
    std::lock_guard<std::mutex> lk(_impl->rulesMutex);
    std::unordered_map<std::string,
        std::shared_ptr<const Impl::DeclRule> >::const_iterator it =
        _impl->rules.find(name);
    return it != _impl->rules.end() && it->second->strategy == "autoIncrement";
}

// ── Resolve 路由 ──
uint64_t AutoComputeProvider::Resolve(const std::string& name, int byteWidth, const BuildContext& ctx) {
    if (byteWidth < 1) byteWidth = 1;
    if (byteWidth > 8) byteWidth = 8;
    // 持锁取规则快照 (shared_ptr), 之后免锁使用 — 策略执行不持锁.
    std::shared_ptr<const Impl::DeclRule> drHold;
    {
        std::lock_guard<std::mutex> lk(_impl->rulesMutex);
        std::unordered_map<std::string,
            std::shared_ptr<const Impl::DeclRule> >::const_iterator it =
            _impl->rules.find(name);
        if (it != _impl->rules.end()) drHold = it->second;
    }
    if (!drHold) {
        return Next(name, byteWidth);
    }
    const Impl::DeclRule& dr = *drHold;
    // ⚠ 已知风险 (已登记, 未修): 下方 catch (...) 会把**任何**策略异常静默吞掉
    //   并退化为自增计数值 —— 即产出一个"构建成功但内容错误"的请求
    //   (例: algo 拼错、frameSlice 缺 frameSoFar)。
    //   为何暂不算活跃缺陷:
    //     1) crc 的 algo 已由 ConfigDeepValidator 强制校验, 拼错在保存期即被拒;
    //     2) frameSlice 的 frameSoFar 由 RenderTemplate 恒设 (见该处 ctx.frameSoFar = &out);
    //     3) autoIncrement 策略本身不抛。
    //   为何不直接 rethrow: Resolve 的两条调用链 (RequestBuilder::RenderTemplate /
    //   ResolveAutoIncrementParameters) 都没有异常边界, 异常会从 asio 回调逸出
    //   并触发 std::terminate。正确修法是先给调用链加异常边界, 或把 Resolve 改为
    //   返回 Core::Expected —— 属独立工作项。
    try {
        if      (dr.strategy == "autoIncrement") return ExecAutoIncrement(name, byteWidth, dr.params);
        else if (dr.strategy == "frameSlice")    return ExecFrameSlice(name, byteWidth, dr.params, ctx);
        else if (dr.strategy == "expr")          return ExecExpr(name, byteWidth, dr.params, ctx);
        else if (dr.strategy == "crc")           return ExecCrc(name, byteWidth, dr.params, ctx);
        return Next(name, byteWidth);
    } catch (...) {
        return Next(name, byteWidth);
    }
}

// 4 个策略在 Impl 内部
//   autoIncrement 语义: 首次请求返回 seed 值; 之后每次 +1.
//   不带 seed: 计数器从 0 起, 首次返回 1.
//   带 seed=1: 首次返回 1, 然后 2, 3, ...
uint64_t AutoComputeProvider::ExecAutoIncrement(const std::string& name, int byteWidth, const JVal& params) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(_impl->countersMutex);
        auto it = _impl->counters.find(name);
        if (it == _impl->counters.end()) {
            // 首次驻留: 按 seed 初始化 (缺少 seed 即 0).
            const JVal* s = params.Find("seed");
            uint64_t seedVal = s ? JToU64(*s, 0) : 0;
            _impl->counters[name] = seedVal;
            first = true;
        }
    }
    if (first) {
        // 首次调用直接返回 seed 值, 不再 +1 (后续调用才走 Next 自增).
        std::lock_guard<std::mutex> lk(_impl->countersMutex);
        return _impl->counters[name];
    }
    return Next(name, byteWidth);
}
uint64_t AutoComputeProvider::ExecFrameSlice(const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx) {
    (void)name; (void)byteWidth;
    if (!ctx.frameSoFar) throw std::runtime_error("frameSlice: missing frameSoFar");
    const auto& frame = *ctx.frameSoFar;
    SlicePos from = ParseSlicePos(params.Str("from", "0"));
    SlicePos to   = ParseSlicePos(params.Str("to",   "end"));
    if (!from.valid) throw std::runtime_error("frameSlice: invalid 'from'");
    if (!to.valid)   throw std::runtime_error("frameSlice: invalid 'to'");
    int32_t f = ResolveSlicePos(from, frame.size());
    int32_t t = ResolveSlicePos(to,   frame.size());
    std::string as = params.Str("as", "length");
    if (as == "length") {
        int32_t n = t - f; if (n < 0) n = 0;
        return (uint64_t)n;
    }
    if (as == "bytes" || as == "value") {
        std::string endian = params.Str("endian", "big");
        return ReadSliceValue(frame.data(), frame.size(), f, t, endian);
    }
    throw std::runtime_error("frameSlice: 'as' must be length/bytes/value, got: " + as);
}
uint64_t AutoComputeProvider::ExecExpr(const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx) {
    (void)name; (void)byteWidth;
    if (!params.Has("value")) {
        throw std::runtime_error("expr: missing 'value' string");
    }
    std::string exprStr = params.Str("value", "");
    if (exprStr.empty()) {
        throw std::runtime_error("expr: value string is empty");
    }
    auto lookup = [ctx](const std::string& varName, bool& found) -> uint64_t {
        found = false;
        // expr 内置两个 magic 变量
        //   __frameLen = frameSoFar 当前长度 (字节数, 不含本段自身)
        //   __frameEnd = 同 __frameLen, 语义更清晰
        if (varName == Core::kExprMagicFrameLen || varName == Core::kExprMagicFrameEnd) {
            if (ctx.frameSoFar) { found = true; return (uint64_t)ctx.frameSoFar->size(); }
            return 0;
        }
        if (!ctx.variables) return 0;
        auto it = ctx.variables->find(varName);
        if (it == ctx.variables->end()) return 0;
        found = true;
        return it->second;
    };
    std::shared_ptr<MiniExpression::Parser> parser;
    {
        // 仅查表加锁; 未命中时的 Parse 与随后的 Evaluate 均在锁外 —
        // 并发重复解析同一表达式是良性的 (结果等价, 后者覆盖前者).
        std::lock_guard<std::mutex> lk(_impl->exprCacheMutex);
        std::unordered_map<std::string,
            std::shared_ptr<MiniExpression::Parser> >::const_iterator cacheIt =
            _impl->exprCache.find(exprStr);
        if (cacheIt != _impl->exprCache.end()) parser = cacheIt->second;
    }
    if (!parser) {
        std::shared_ptr<MiniExpression::Parser> fresh(new MiniExpression::Parser(exprStr));
        fresh->Parse();
        std::lock_guard<std::mutex> lk(_impl->exprCacheMutex);
        _impl->exprCache[exprStr] = fresh;
        parser = fresh;
    }
    uint64_t v = parser->Evaluate(lookup);
    return v;
}
uint64_t AutoComputeProvider::ExecCrc(const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx) {
    (void)name; (void)byteWidth;
    if (!ctx.frameSoFar) throw std::runtime_error("crc: missing frameSoFar");
    const auto& frame = *ctx.frameSoFar;
    // algo 必填 — 不设缺省值: 任何缺省都等于把某协议的选择烘焙进 Engine.
    //   校验器 (ConfigDeepValidator) 已强制 params.algo 存在且为受支持算法;
    //   此处仅兜底, 避免无 algo 时静默套用某个协议的算法.
    std::string algo = params.Str("algo", "");
    if (algo.empty()) {
        throw std::runtime_error("crc: 缺少必填参数 algo (可选: "
                                 "crc16-modbus / crc16-ccitt / crc32)");
    }
    CrcTable t = GetCrcTable(algo);
    SlicePos from = ParseSlicePos(params.Str("from", "0"));
    SlicePos to   = ParseSlicePos(params.Str("to",   "end"));
    int32_t f = ResolveSlicePos(from, frame.size());
    int32_t end = ResolveSlicePos(to, frame.size());
    if (f < 0) f = 0;
    if (end > (int32_t)frame.size()) end = (int32_t)frame.size();
    if (end < f) end = f;
    uint32_t crc = t.Compute(frame.data() + f, end - f);
    std::string byteOrder = params.Str("byteOrder", "little");
    int crcWidth = (algo.find("crc32") != std::string::npos) ? 4 : 2;
    if (byteOrder == "little") {
        return crc & ((crcWidth == 4) ? 0xFFFFFFFFull : 0xFFFFull);
    } else {
        uint64_t v = 0;
        for (int i = 0; i < crcWidth; ++i) {
            v |= ((uint64_t)crc & 0xFF) << (i * 8);
            crc >>= 8;
        }
        return v;
    }
}

bool AutoComputeProvider::ResolveDerivedLength(
        const std::string& expr,
        const std::unordered_map<std::string, uint32_t>* inputs,
        const std::unordered_map<std::string, uint32_t>* varLen,
        const TemplateLayout* layout,
        uint32_t& out,
        std::string* errMsg) {
    if (errMsg) errMsg->clear();
    // {name:len} 引用 inputs 变量的字节长度 (载荷变量=实际字节数, 其余=模板渲染宽度);
    //        普通名引用查 inputs 值池. 无 payload/count 保留名.
    // 模板结构原语 (ADR-0012 §1.1): {Frame:fixed} 查 layout->fixedTotal;
    //        {name:offset} 查 layout->offsets (首现偏移). layout 缺失 → 两原语不可解析.
    try {
        MiniExpression::Parser p(expr);
        p.Parse();
        auto lookup = [&](const std::string& n, bool& found) -> uint64_t {
            // {name:prop} 形式 → 按属性分发
            if (n.size() > 2 && n.front() == '{' && n.back() == '}') {
                const std::string inner = n.substr(1, n.size() - 2);
                const size_t colon = inner.find(':');
                if (colon == std::string::npos) {
                    found = false;
                    return 0;
                }
                const std::string name = inner.substr(0, colon);
                const std::string prop = inner.substr(colon + 1);
                if (prop == Core::kPropLength) {
                    if (varLen) {
                        std::unordered_map<std::string, uint32_t>::const_iterator it = varLen->find(name);
                        if (it != varLen->end()) { found = true; return it->second; }
                    }
                    found = false;
                    return 0;
                }
                if (prop == Core::kPropOffset) {
                    if (layout) {
                        std::unordered_map<std::string, uint32_t>::const_iterator it = layout->offsets.find(name);
                        if (it != layout->offsets.end()) { found = true; return it->second; }
                    }
                    found = false;
                    return 0;
                }
                if (prop == Core::kPropFixed && name == Core::kFramePrimitiveName) {
                    if (layout) { found = true; return layout->fixedTotal; }
                    found = false;
                    return 0;
                }
                found = false;
                return 0;
            }
            // 普通变量 → 查值池
            if (inputs) {
                std::unordered_map<std::string, uint32_t>::const_iterator it = inputs->find(n);
                if (it != inputs->end()) { found = true; return it->second; }
            }
            found = false;
            return 0;
        };
        out = static_cast<uint32_t>(p.Evaluate(lookup) & 0xFFFFFFFFull);
        return true;
    } catch (const std::exception& e) {
        if (errMsg) *errMsg = e.what();
        return false;
    } catch (...) {
        if (errMsg) *errMsg = "未知求值错误";
        return false;
    }
}

}} // namespace MyProt::Engine

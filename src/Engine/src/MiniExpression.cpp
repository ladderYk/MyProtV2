// src/Engine/src/MiniExpression.cpp
// 极简算术表达式 — 递归下降 + 节点求值
#include "MyProt/Engine/MiniExpression.hpp"
#include <cctype>
#include <stdexcept>
#include <sstream>

namespace MyProt { namespace Engine { namespace MiniExpression {

// 节点构造助手 (unique_ptr 不可拷贝, 聚合初始化不能用 brace-list)
static std::unique_ptr<Node> MakeNum(uint64_t v) {
    auto n = std::unique_ptr<Node>(new Node());
    n->type = Node::Num;
    n->num = v;
    return n;
}
static std::unique_ptr<Node> MakeVar(const std::string& name) {
    auto n = std::unique_ptr<Node>(new Node());
    n->type = Node::Var;
    n->text = name;
    return n;
}
static std::unique_ptr<Node> MakeUnary(char op, std::unique_ptr<Node> rhs) {
    auto n = std::unique_ptr<Node>(new Node());
    n->type = Node::Unary;
    n->op = op;
    n->rhs = std::move(rhs);
    return n;
}
static std::unique_ptr<Node> MakeBinary(char op, std::unique_ptr<Node> lhs, std::unique_ptr<Node> rhs) {
    auto n = std::unique_ptr<Node>(new Node());
    n->type = Node::Binary;
    n->op = op;
    n->lhs = std::move(lhs);
    n->rhs = std::move(rhs);
    return n;
}

Parser::Parser(const std::string& expr) : expr_(expr) {}
Parser::~Parser() = default;

void Parser::SkipWs() {
    while (pos_ < expr_.size() && (expr_[pos_] == ' ' || expr_[pos_] == '\t')) pos_++;
}
bool Parser::Peek(char c) {
    SkipWs();
    return pos_ < expr_.size() && expr_[pos_] == c;
}
bool Parser::Consume(char c) {
    SkipWs();
    if (pos_ < expr_.size() && expr_[pos_] == c) { pos_++; return true; }
    return false;
}

// 优先级从低到高: Expr(+-) → Term(*/%) → BitOr(|) → BitXor(^) → BitAnd(&) → Unary(-~) → Primary
std::unique_ptr<Node> Parser::ParseExpr() {
    auto left = ParseTerm();
    while (true) {
        if (Peek('+') || Peek('-')) {
            char op = expr_[pos_];
            ++pos_;
            auto right = ParseTerm();
            left = MakeBinary(op, std::move(left), std::move(right));
        } else break;
    }
    return left;
}
std::unique_ptr<Node> Parser::ParseTerm() {
    auto left = ParseBitOr();
    while (true) {
        char op = 0;
        if (Consume('*')) op = '*';
        else if (Consume('/')) op = '/';
        else if (Consume('%')) op = '%';
        else break;
        auto right = ParseBitOr();
        left = MakeBinary(op, std::move(left), std::move(right));
    }
    return left;
}
std::unique_ptr<Node> Parser::ParseBitOr() {
    auto left = ParseBitXor();
    while (Consume('|')) {
        auto right = ParseBitXor();
        left = MakeBinary('|', std::move(left), std::move(right));
    }
    return left;
}
std::unique_ptr<Node> Parser::ParseBitXor() {
    auto left = ParseBitAnd();
    while (Consume('^')) {
        auto right = ParseBitAnd();
        left = MakeBinary('^', std::move(left), std::move(right));
    }
    return left;
}
std::unique_ptr<Node> Parser::ParseBitAnd() {
    auto left = ParseUnary();
    while (Consume('&')) {
        auto right = ParseUnary();
        left = MakeBinary('&', std::move(left), std::move(right));
    }
    return left;
}
std::unique_ptr<Node> Parser::ParseUnary() {
    if (Consume('-')) {
        auto operand = ParseUnary();
        return MakeUnary('-', std::move(operand));
    }
    if (Consume('~')) {
        auto operand = ParseUnary();
        return MakeUnary('~', std::move(operand));
    }
    if (Consume('+')) {
        // 一元 + 直接透传
        return ParseUnary();
    }
    return ParsePrimary();
}
std::unique_ptr<Node> Parser::ParsePrimary() {
    SkipWs();
    if (pos_ >= expr_.size()) {
        throw Error("MiniExpression: 意外的表达式末尾");
    }
    if (Consume('(')) {
        auto inner = ParseExpr();
        if (!Consume(')')) {
            throw Error("MiniExpression: 缺少右括号");
        }
        return inner;
    }
    if (expr_[pos_] == '{') {
        return ParseLenToken();
    }
    if (std::isdigit(static_cast<unsigned char>(expr_[pos_]))) {
        return ParseNumber();
    }
    if (std::isalpha(static_cast<unsigned char>(expr_[pos_])) || expr_[pos_] == '_') {
        return ParseIdent();
    }
    throw Error(std::string("MiniExpression: 意外的字符 '") + expr_[pos_] + "'");
}
std::unique_ptr<Node> Parser::ParseNumber() {
    SkipWs();
    size_t start = pos_;
    int base = 10;
    if (expr_[pos_] == '0' && pos_ + 1 < expr_.size() && (expr_[pos_+1] == 'x' || expr_[pos_+1] == 'X')) {
        base = 16;
        pos_ += 2;
        while (pos_ < expr_.size() && std::isxdigit(static_cast<unsigned char>(expr_[pos_]))) pos_++;
    } else {
        while (pos_ < expr_.size() && (std::isdigit(static_cast<unsigned char>(expr_[pos_])) || expr_[pos_] == '.')) pos_++;
    }
    std::string numStr = expr_.substr(start, pos_ - start);
    if (numStr.empty()) throw Error("MiniExpression: 数字格式错误");
    uint64_t val = 0;
    try {
        if (base == 16) {
            std::string hex = numStr.substr(2);
            val = std::stoull(hex, nullptr, 16);
        } else {
            val = std::stoull(numStr, nullptr, 10);
        }
    } catch (...) {
        throw Error("MiniExpression: 数字溢出或格式错误: " + numStr);
    }
    return MakeNum(val);
}
std::unique_ptr<Node> Parser::ParseIdent() {
    SkipWs();
    size_t start = pos_;
    while (pos_ < expr_.size() && (std::isalnum(static_cast<unsigned char>(expr_[pos_])) || expr_[pos_] == '_')) pos_++;
    std::string name = expr_.substr(start, pos_ - start);
    if (name.empty()) throw Error("MiniExpression: 变量名为空");
    return MakeVar(name);
}

// v1.21: {name:len} — 引用 inputs 变量的字节长度 (载荷变量=实际字节数, 其余=模板渲染宽度)
//   Var 节点文本保留 "{name:prop}" 原样, 求值端按 prop 分发查表.
// v1.25 (ADR-0012 §1.1): 属性扩展 — offset (占位符首现偏移) / fixed (Frame: 模板固定段总宽);
//   分发在求值端 (ResolveDerivedLength lookup), 词法只负责接受属性名集合.
std::unique_ptr<Node> Parser::ParseLenToken() {
    SkipWs();
    if (!Consume('{')) throw Error("MiniExpression: 缺少 '{'");
    SkipWs();
    size_t start = pos_;
    while (pos_ < expr_.size() && (std::isalnum(static_cast<unsigned char>(expr_[pos_])) || expr_[pos_] == '_')) pos_++;
    std::string name = expr_.substr(start, pos_ - start);
    if (name.empty()) throw Error("MiniExpression: {name:prop} 变量名为空");
    if (!Consume(':')) throw Error("MiniExpression: {name:prop} 缺少 ':'");
    SkipWs();
    size_t pstart = pos_;
    while (pos_ < expr_.size() && (std::isalnum(static_cast<unsigned char>(expr_[pos_])) || expr_[pos_] == '_')) pos_++;
    std::string prop = expr_.substr(pstart, pos_ - pstart);
    if (prop.empty()) throw Error("MiniExpression: {name:prop} 缺少属性名");
    if (!Consume('}')) throw Error("MiniExpression: {name:prop} 缺少 '}'");
    if (prop != "len" && prop != "offset" && prop != "fixed") {
        throw Error("MiniExpression: 不支持的属性 \"" + prop + "\" (支持 len/offset/fixed)");
    }
    return MakeVar("{" + name + ":" + prop + "}");
}

void Parser::Parse() {
    pos_ = 0;
    root_ = ParseExpr();
    SkipWs();
    if (pos_ != expr_.size()) {
        throw Error(std::string("MiniExpression: 解析后剩余字符: ") + expr_.substr(pos_));
    }
    parsed_ = true;
}

uint64_t Parser::EvalNode(const Node& n, const VarLookup& lookup) const {
    switch (n.type) {
        case Node::Num: return n.num;
        case Node::Var: {
            bool found = true;
            uint64_t v = lookup(n.text, found);
            if (!found) throw Error("MiniExpression: 未知变量: " + n.text);
            return v;
        }
        case Node::Unary: {
            uint64_t v = EvalNode(*n.rhs, lookup);
            switch (n.op) {
                case '-': return (uint64_t)(-(int64_t)v);
                case '~': return ~v;
                default: throw Error("MiniExpression: 未知一元运算符");
            }
        }
        case Node::Binary: {
            uint64_t a = EvalNode(*n.lhs, lookup);
            uint64_t b = EvalNode(*n.rhs, lookup);
            switch (n.op) {
                case '+': return a + b;
                case '-': return a - b;
                case '*': return a * b;
                case '/': if (b == 0) throw Error("MiniExpression: 除零"); return a / b;
                case '%': if (b == 0) throw Error("MiniExpression: 模零"); return a % b;
                case '&': return a & b;
                case '|': return a | b;
                case '^': return a ^ b;
                default: throw Error("MiniExpression: 未知二元运算符");
            }
        }
    }
    throw Error("MiniExpression: 未知节点类型");
}

uint64_t Parser::Evaluate(const VarLookup& lookup) const {
    if (!parsed_) throw Error("MiniExpression: 尚未 Parse");
    if (!root_) throw Error("MiniExpression: AST 为空");
    return EvalNode(*root_, lookup);
}

}}} // namespace MyProt::Engine::MiniExpression

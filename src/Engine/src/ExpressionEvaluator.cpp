// src/Engine/src/ExpressionEvaluator.cpp
// 表达式求值器实现 — 递归下降 (resp[N] / resp[A:B] / C 优先级, C++11 ADR-0010 §5)
// 错误传播: 解析/求值异常统一在入口捕获转为 Expected 错误。

#include "MyProt/Engine/ExpressionEvaluator.hpp"
#include <cctype>
#include <cstdlib>
#include <stdexcept>

namespace MyProt { namespace Engine {

const ExpressionEvaluator::Token& ExpressionEvaluator::Current() const {
    static const Token kEof;   // Eof 哨兵
    if (_pos >= _tokens->size()) return kEof;
    return (*_tokens)[_pos];
}

const ExpressionEvaluator::Token& ExpressionEvaluator::Advance() {
    const Token& t = Current();
    if (_pos < _tokens->size()) ++_pos;
    return t;
}

bool ExpressionEvaluator::Match(TokenType t) {
    if (Current().type == t) { Advance(); return true; }
    return false;
}

std::vector<ExpressionEvaluator::Token> ExpressionEvaluator::Tokenize(const std::string& expr) {
    std::vector<Token> out;
    const size_t n = expr.size();
    size_t i = 0;

    while (i < n) {
        const char c = expr[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }

        // resp[N] / resp[A:B] (大小写不敏感前缀)
        if ((c == 'r' || c == 'R') && n - i >= 5 &&
            expr.compare(i, 4, "resp") == 0 && expr[i + 4] == '[') {
            const size_t close = expr.find(']', i + 5);
            if (close == std::string::npos) {
                throw std::runtime_error("resp[ 缺少匹配的 ]");
            }
            std::string inner = expr.substr(i + 5, close - i - 5);
            Token t;
            const size_t colon = inner.find(':');
            if (colon != std::string::npos) {
                t.type = TokenType::RespRange;
                t.number = std::strtoll(inner.substr(0, colon).c_str(), nullptr, 0);
                t.number2 = std::strtoll(inner.substr(colon + 1).c_str(), nullptr, 0);
            } else {
                t.type = TokenType::RespIndex;
                t.number = std::strtoll(inner.c_str(), nullptr, 0);
            }
            out.push_back(t);
            i = close + 1;
            continue;
        }

        // 数字字面量: 十进制 / 0x 十六进制
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            size_t j = i;
            if (c == '0' && i + 1 < n && (expr[i + 1] == 'x' || expr[i + 1] == 'X')) {
                j = i + 2;
                while (j < n && std::isxdigit(static_cast<unsigned char>(expr[j])) != 0) ++j;
            } else {
                while (j < n && std::isdigit(static_cast<unsigned char>(expr[j])) != 0) ++j;
            }
            Token t;
            t.type = TokenType::Number;
            t.number = std::strtoll(expr.substr(i, j - i).c_str(), nullptr, 0);
            out.push_back(t);
            i = j;
            continue;
        }

        // 双字符运算符 (先长后短)
        if (i + 1 < n) {
            const std::string two = expr.substr(i, 2);
            TokenType tt = TokenType::Eof;
            if      (two == "||") tt = TokenType::OpOr;
            else if (two == "&&") tt = TokenType::OpAnd;
            else if (two == "<<") tt = TokenType::OpShl;
            else if (two == ">>") tt = TokenType::OpShr;
            else if (two == "==") tt = TokenType::OpEq;
            else if (two == "!=") tt = TokenType::OpNe;
            else if (two == "<=") tt = TokenType::OpLe;
            else if (two == ">=") tt = TokenType::OpGe;
            if (tt != TokenType::Eof) {
                Token t; t.type = tt;
                out.push_back(t);
                i += 2;
                continue;
            }
        }

        // 单字符运算符 / 括号
        TokenType tt = TokenType::Eof;
        switch (c) {
            case '|': tt = TokenType::OpBitOr;   break;
            case '^': tt = TokenType::OpBitXor;  break;
            case '&': tt = TokenType::OpBitAnd;  break;
            case '<': tt = TokenType::OpLt;      break;
            case '>': tt = TokenType::OpGt;      break;
            case '+': tt = TokenType::OpAdd;     break;
            case '-': tt = TokenType::OpSub;     break;
            case '*': tt = TokenType::OpMul;     break;
            case '/': tt = TokenType::OpDiv;     break;
            case '%': tt = TokenType::OpMod;     break;
            case '!': tt = TokenType::OpNot;     break;
            case '~': tt = TokenType::OpBitNot;  break;
            case '(': tt = TokenType::LParen;    break;
            case ')': tt = TokenType::RParen;    break;
            default:
                throw std::runtime_error(std::string("非法字符: ") + c);
        }
        Token t; t.type = tt;
        out.push_back(t);
        ++i;
    }
    return out;
}

// ── 递归下降: 每层一个方法, 布尔结果以非零表示 ──

int64_t ExpressionEvaluator::ParseOr() {
    int64_t v = ParseAnd();
    while (Match(TokenType::OpOr)) {
        const int64_t r = ParseAnd();
        v = (v != 0 || r != 0) ? 1 : 0;
    }
    return v;
}

int64_t ExpressionEvaluator::ParseAnd() {
    int64_t v = ParseBitOr();
    while (Match(TokenType::OpAnd)) {
        const int64_t r = ParseBitOr();
        v = (v != 0 && r != 0) ? 1 : 0;
    }
    return v;
}

int64_t ExpressionEvaluator::ParseBitOr() {
    int64_t v = ParseBitXor();
    while (Match(TokenType::OpBitOr)) v |= ParseBitXor();
    return v;
}

int64_t ExpressionEvaluator::ParseBitXor() {
    int64_t v = ParseBitAnd();
    while (Match(TokenType::OpBitXor)) v ^= ParseBitAnd();
    return v;
}

int64_t ExpressionEvaluator::ParseBitAnd() {
    int64_t v = ParseEquality();
    while (Match(TokenType::OpBitAnd)) v &= ParseEquality();
    return v;
}

int64_t ExpressionEvaluator::ParseEquality() {
    int64_t v = ParseComparison();
    for (;;) {
        if (Match(TokenType::OpEq)) {
            v = (v == ParseComparison()) ? 1 : 0;
        } else if (Match(TokenType::OpNe)) {
            v = (v != ParseComparison()) ? 1 : 0;
        } else {
            return v;
        }
    }
}

int64_t ExpressionEvaluator::ParseComparison() {
    int64_t v = ParseShift();
    for (;;) {
        if (Match(TokenType::OpLt))       v = (v <  ParseShift()) ? 1 : 0;
        else if (Match(TokenType::OpGt))  v = (v >  ParseShift()) ? 1 : 0;
        else if (Match(TokenType::OpLe))  v = (v <= ParseShift()) ? 1 : 0;
        else if (Match(TokenType::OpGe))  v = (v >= ParseShift()) ? 1 : 0;
        else return v;
    }
}

int64_t ExpressionEvaluator::ParseShift() {
    int64_t v = ParseAddSub();
    for (;;) {
        if (Match(TokenType::OpShl)) v = static_cast<int64_t>(
            static_cast<uint64_t>(v) << static_cast<unsigned>(ParseAddSub() & 63));
        else if (Match(TokenType::OpShr)) v >>= static_cast<int>(ParseAddSub() & 63);
        else return v;
    }
}

int64_t ExpressionEvaluator::ParseAddSub() {
    int64_t v = ParseMulDiv();
    for (;;) {
        if (Match(TokenType::OpAdd))      v += ParseMulDiv();
        else if (Match(TokenType::OpSub)) v -= ParseMulDiv();
        else return v;
    }
}

int64_t ExpressionEvaluator::ParseMulDiv() {
    int64_t v = ParseUnary();
    for (;;) {
        if (Match(TokenType::OpMul)) {
            v *= ParseUnary();
        } else if (Match(TokenType::OpDiv)) {
            const int64_t r = ParseUnary();
            if (r == 0) throw std::runtime_error("除零");
            v /= r;
        } else if (Match(TokenType::OpMod)) {
            const int64_t r = ParseUnary();
            if (r == 0) throw std::runtime_error("模零");
            v %= r;
        } else {
            return v;
        }
    }
}

int64_t ExpressionEvaluator::ParseUnary() {
    if (Match(TokenType::OpNot))    return (ParseUnary() == 0) ? 1 : 0;
    if (Match(TokenType::OpBitNot)) return ~ParseUnary();
    if (Match(TokenType::OpSub))    return -ParseUnary();
    if (Match(TokenType::OpAdd))    return ParseUnary();
    return ParsePrimary();
}

int64_t ExpressionEvaluator::ParsePrimary() {
    const Token& t = Current();
    if (t.type == TokenType::Number) {
        Advance();
        return t.number;
    }
    if (t.type == TokenType::RespIndex) {
        Advance();
        if (t.number < 0 || static_cast<size_t>(t.number) >= _response.size) {
            throw std::runtime_error("resp 索引越界");
        }
        return _response.data[static_cast<size_t>(t.number)];
    }
    if (t.type == TokenType::RespRange) {
        throw std::runtime_error("resp[A:B] 切片不能用于标量表达式");
    }
    if (t.type == TokenType::LParen) {
        Advance();
        const int64_t v = ParseOr();
        if (!Match(TokenType::RParen)) throw std::runtime_error("缺少 )");
        return v;
    }
    throw std::runtime_error("意外的表达式标记");
}

Core::Expected<bool> ExpressionEvaluator::EvaluateCondition(
    const std::string& expr, Core::ByteView response) {
    try {
        if (expr.empty()) {
            return Core::Unexpected(Core::Error::Code::ParseError, "空条件表达式");
        }
        std::vector<Token> tokens = Tokenize(expr);
        _tokens = &tokens;
        _pos = 0;
        _response = response;

        const int64_t v = ParseOr();
        if (_pos != tokens.size()) {
            throw std::runtime_error("表达式存在未消费的标记");
        }
        return v != 0;
    } catch (const std::exception& e) {
        return Core::Unexpected(Core::Error::Code::ParseError, e.what(), expr);
    }
}

Core::Expected<int> ExpressionEvaluator::EvaluateLength(
    const std::string& expr, Core::ByteView response) {
    try {
        if (expr.empty()) {
            return Core::Unexpected(Core::Error::Code::ParseError, "空长度表达式");
        }
        std::vector<Token> tokens = Tokenize(expr);
        _tokens = &tokens;
        _pos = 0;
        _response = response;

        const int64_t v = ParseOr();
        if (_pos != tokens.size()) {
            throw std::runtime_error("表达式存在未消费的标记");
        }
        return static_cast<int>(v);
    } catch (const std::exception& e) {
        return Core::Unexpected(Core::Error::Code::ParseError, e.what(), expr);
    }
}

}} // namespace MyProt::Engine

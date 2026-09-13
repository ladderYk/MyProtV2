// src/Engine/include/MyProt/Engine/ExpressionEvaluator.hpp
// 表达式求值器 — validCondition / dataLengthExpr 等 (modules/02_Engine.md)
// 递归下降解析器, C 优先级对齐 (ADR-0003 KI-13)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Engine {

/// 表达式求值器
/// 支持 resp[N] 字节引用, resp[A:B] 切片, 比较运算, 算术/逻辑运算
/// EBNF 优先级: || < && < | < ^ < & < == != < < > <= >= < << >> < + - < * / % < 一元(! - ~)
class ExpressionEvaluator {
public:
    /// 求值条件表达式 (如 "resp[7] == 0x03")
    Core::Expected<bool> EvaluateCondition(
        const std::string& expr,
        Core::ByteView response);

    /// 求值长度表达式 (如 "resp[2]")
    Core::Expected<int> EvaluateLength(
        const std::string& expr,
        Core::ByteView response);

private:
    // ── 词法分析 ──
    enum class TokenType {
        Number,         // 整数字面量 (十进制/十六进制)
        RespIndex,      // resp[N] — 值存 number
        RespRange,      // resp[A:B] — 值存 number, number2 存 B
        LParen, RParen,
        OpOr, OpAnd,    // || &&
        OpBitOr, OpBitXor, OpBitAnd,  // | ^ &
        OpEq, OpNe,     // == !=
        OpLt, OpGt, OpLe, OpGe,  // < > <= >=
        OpShl, OpShr,   // << >>
        OpAdd, OpSub,   // + -
        OpMul, OpDiv, OpMod,  // * / %
        OpNot, OpBitNot, // ! ~
        Eof
    };

    struct Token {
        TokenType type;
        int64_t number;     // Number / RespIndex 的值
        int64_t number2;    // RespRange 的结束索引
        Token() : type(TokenType::Eof), number(0), number2(0) {}
    };

    // ── 递归下降: 每层一个方法, 返回 int64_t ──
    // 布尔结果: 非零 = true
    const std::vector<Token>* _tokens;
    size_t _pos;
    Core::ByteView _response;

    std::vector<Token> Tokenize(const std::string& expr);
    int64_t ParseOr();
    int64_t ParseAnd();
    int64_t ParseBitOr();
    int64_t ParseBitXor();
    int64_t ParseBitAnd();
    int64_t ParseEquality();
    int64_t ParseComparison();
    int64_t ParseShift();
    int64_t ParseAddSub();
    int64_t ParseMulDiv();
    int64_t ParseUnary();
    int64_t ParsePrimary();
    const Token& Current() const;
    const Token& Advance();
    bool Match(TokenType t);
};

}} // namespace MyProt::Engine

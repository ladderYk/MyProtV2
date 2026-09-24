// src/Engine/include/MyProt/Engine/ExpressionEvaluator.hpp
// Expression evaluator - validCondition / dataLengthExpr etc. (modules/02_Engine.md)
// Recursive-descent parser, C precedence-aligned (ADR-0003 KI-13)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Engine {

/// Expression evaluator
/// Supports resp[N] byte references, resp[A:B] slices, comparison operators, arithmetic/logical operators
/// EBNF precedence: || < && < | < ^ < & < == != < < > <= >= < << >> < + - < * / % < unary(! - ~)
class ExpressionEvaluator {
public:
    /// Evaluate a condition expression (e.g. "resp[7] == 0x03")
    Core::Expected<bool> EvaluateCondition(
        const std::string& expr,
        Core::ByteView response);

    /// Evaluate a length expression (e.g. "resp[2]")
    Core::Expected<int> EvaluateLength(
        const std::string& expr,
        Core::ByteView response);

private:
    // ── Lexical analysis ──
    enum class TokenType {
        Number,         // integer literal (decimal/hexadecimal)
        RespIndex,      // resp[N] - value stored in number
        RespRange,      // resp[A:B] - value stored in number, number2 stores B
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
        int64_t number;     // value of Number / RespIndex
        int64_t number2;    // end index of RespRange
        Token() : type(TokenType::Eof), number(0), number2(0) {}
    };

    // ── Recursive descent: one method per level, returns int64_t ──
    // Boolean result: non-zero = true
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

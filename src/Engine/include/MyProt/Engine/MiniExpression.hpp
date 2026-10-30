// src/Engine/include/MyProt/Engine/MiniExpression.hpp
// Minimal arithmetic + condition expression evaluator (zero dependency)
//   Arithmetic: numbers (10/0x1F), variable names, + - * / % & | ^ ~ ( ), unary +/-/~
//   Shifts (v1.29): << >> — same precedence as C (looser than +/-, tighter than comparisons)
//   Condition (v1.28): == != > < >= <= && || ! and resp[N] response-byte indexing
//     — comparison/logical nodes evaluate to 0/1; resp[N] resolves via the VarLookup
//       callback using the canonical name "resp[<index>]" (index may itself be an expression).
//   Builtin functions (v1.33): hi8(x) lo8(x) hi16(x) lo16(x) — byte/word extraction,
//     replacing the error-prone "% 256" / "/ 256 % 256" arithmetic in derivedLength exprs.
//   Does not support: other function calls, strings, ternary (use autoCompute strategies for those)
//
// Usage:
//   MiniExpression::Parser p("RegisterCount * 2 + (1 & 0xFF)");
//   p.Parse();   // throws MiniExpression::Error on failure
//   uint64_t v = p.Evaluate([](const std::string& name, bool& found) -> uint64_t {
//       auto it = vars.find(name); if (it == vars.end()) { found = false; return 0; }
//       found = true; return it->second;
//   });
//   // condition example: "resp[7] == 0x03 && resp[8] != 0xFF" → 1/0
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <functional>
#include <vector>
#include <unordered_map>
#include <memory>

namespace MyProt { namespace Engine { namespace MiniExpression {

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Variable-lookup callback: given a variable name, return its value; if not found, set found to false
using VarLookup = std::function<uint64_t(const std::string&, bool& found)>;

/// AST node (public so the implementation .cpp can construct/evaluate directly)
struct Node {
    enum Type { Num, Var, Resp, Func, Unary, Binary };
    Type type;
    std::string text;   // for Var (variable name) / Func (builtin function name)
    uint64_t num = 0;   // for Num
    char op = 0;        // Unary: '-','~','!'  |  Binary arithmetic: '+','-','*','/','%','&','|','^'
                        // Binary shift: 's' (<<)  'S' (>>)
                        // Binary condition (single-char codes, no collision with the above):
                        //   '=' (==)  'n' (!=)  '>'  '<'  'G' (>=)  'L' (<=)  'A' (&&)  'O' (||)
    std::unique_ptr<Node> lhs, rhs;  // Resp: lhs holds the index expression; Func: lhs holds the single argument
};

/// Whether name is a supported builtin function (hi8 / lo8 / hi16 / lo16).
bool IsBuiltinFunction(const std::string& name);

class Parser {
public:
    explicit Parser(const std::string& expr);
    ~Parser();

    /// Parse into an AST (throws Error on failure)
    void Parse();

    /// Evaluate (given a variable table)
    /// @param lookup variable finder; an unknown variable name throws Error("unknown variable: X")
    uint64_t Evaluate(const VarLookup& lookup) const;

    /// Whether parsing succeeded
    bool IsParsed() const { return parsed_; }

private:
    std::string expr_;
    size_t pos_ = 0;
    bool parsed_ = false;
    std::unique_ptr<Node> root_;

    // Parser (recursive descent)
    // Precedence low→high (C-aligned, ADR-0003 KI-13): Or || → And && → BitOr | → BitXor ^
    //                       → BitAnd & → Equality ==/!= → Relational >/</>=/<= → Shift << >>
    //                       → Expr +/- → Term */% → Unary (prefix) → Primary
    void SkipWs();
    bool Peek(char c);
    bool Consume(char c);
    std::unique_ptr<Node> ParseOr();         // ||
    std::unique_ptr<Node> ParseAnd();        // &&
    std::unique_ptr<Node> ParseEquality();   // == !=
    std::unique_ptr<Node> ParseRelational(); // > < >= <=
    std::unique_ptr<Node> ParseShift();      // << >>
    std::unique_ptr<Node> ParseExpr();       // + -
    std::unique_ptr<Node> ParseTerm();       // * / %
    std::unique_ptr<Node> ParseBitOr();      // |
    std::unique_ptr<Node> ParseBitXor();     // ^
    std::unique_ptr<Node> ParseBitAnd();     // &
    std::unique_ptr<Node> ParseUnary();      // - ~ + ! (prefix)
    std::unique_ptr<Node> ParsePrimary();    // number/variable/resp[N]/{name:len}/(expr)
    std::unique_ptr<Node> ParseNumber();
    std::unique_ptr<Node> ParseIdent();
    std::unique_ptr<Node> ParseLenToken();   // {name:len} byte-length reference
    std::unique_ptr<Node> ParseCall(const std::string& name);  // builtin function call

    // Evaluation
    uint64_t EvalNode(const Node& n, const VarLookup& lookup) const;
};

}}} // namespace MyProt::Engine::MiniExpression

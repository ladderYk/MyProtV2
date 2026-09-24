// src/Engine/include/MyProt/Engine/MiniExpression.hpp
// Minimal arithmetic expression evaluator (zero dependency)
//   Supports: numbers (10/0x1F), variable names, + - * / % & | ^ ~ ( ), unary minus
//   Does not support: function calls, strings, conditional branches (use autoCompute strategies for those)
//
// Usage:
//   MiniExpression::Parser p("RegisterCount * 2 + (1 & 0xFF)");
//   p.Parse();   // throws MiniExpression::Error on failure
//   uint64_t v = p.Evaluate([](const std::string& name, bool& found) -> uint64_t {
//       auto it = vars.find(name); if (it == vars.end()) { found = false; return 0; }
//       found = true; return it->second;
//   });
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
    enum Type { Num, Var, Unary, Binary };
    Type type;
    std::string text;   // for Var
    uint64_t num = 0;   // for Num
    char op = 0;        // for Unary/Binary: '+','-','*','/','%','&','|','^','~'
    std::unique_ptr<Node> lhs, rhs;
};

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
    void SkipWs();
    bool Peek(char c);
    bool Consume(char c);
    std::unique_ptr<Node> ParseExpr();    // + -
    std::unique_ptr<Node> ParseTerm();    // * / %
    std::unique_ptr<Node> ParseBitOr();   // |
    std::unique_ptr<Node> ParseBitXor();  // ^
    std::unique_ptr<Node> ParseBitAnd();  // &
    std::unique_ptr<Node> ParseUnary();   // - ~ + (prefix)
    std::unique_ptr<Node> ParsePrimary(); // number/variable/{name:len}/(expr)
    std::unique_ptr<Node> ParseNumber();
    std::unique_ptr<Node> ParseIdent();
    std::unique_ptr<Node> ParseLenToken(); // {name:len} byte-length reference

    // Evaluation
    uint64_t EvalNode(const Node& n, const VarLookup& lookup) const;
};

}}} // namespace MyProt::Engine::MiniExpression

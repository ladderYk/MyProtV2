// src/Engine/include/MyProt/Engine/MiniExpression.hpp
// 极简算术表达式求值器 (v1.9, 零依赖)
//   支持: 数字 (10/0x1F), 变量名, + - * / % & | ^ ~ ( ), 一元负号
//   不支持: 函数调用, 字符串, 条件分支 (这些用 autoCompute 策略代替)
//
// 用法:
//   MiniExpression::Parser p("RegisterCount * 2 + (1 & 0xFF)");
//   p.Parse();   // 失败抛 MiniExpression::Error
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

/// 变量查找回调: 给变量名, 返回值; 若找不到, 把 found 置 false
using VarLookup = std::function<uint64_t(const std::string&, bool& found)>;

/// AST 节点 (公开以便实现端 .cpp 可直接构造/求值)
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

    /// 解析为 AST (失败抛 Error)
    void Parse();

    /// 求值 (传入变量表)
    /// @param lookup 变量查找器; 找不到的变量名会抛 Error("unknown variable: X")
    uint64_t Evaluate(const VarLookup& lookup) const;

    /// 是否已成功解析
    bool IsParsed() const { return parsed_; }

private:
    std::string expr_;
    size_t pos_ = 0;
    bool parsed_ = false;
    std::unique_ptr<Node> root_;

    // 解析器 (递归下降)
    void SkipWs();
    bool Peek(char c);
    bool Consume(char c);
    std::unique_ptr<Node> ParseExpr();    // + -
    std::unique_ptr<Node> ParseTerm();    // * / %
    std::unique_ptr<Node> ParseBitOr();   // |
    std::unique_ptr<Node> ParseBitXor();  // ^
    std::unique_ptr<Node> ParseBitAnd();  // &
    std::unique_ptr<Node> ParseUnary();   // - ~ + (前置)
    std::unique_ptr<Node> ParsePrimary(); // 数字/变量/{name:len}/(expr)
    std::unique_ptr<Node> ParseNumber();
    std::unique_ptr<Node> ParseIdent();
    std::unique_ptr<Node> ParseLenToken(); // v1.21: {name:len} 字节长度引用

    // 求值
    uint64_t EvalNode(const Node& n, const VarLookup& lookup) const;
};

}}} // namespace MyProt::Engine::MiniExpression

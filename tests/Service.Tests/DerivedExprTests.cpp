// tests/Service.Tests/DerivedExprTests.cpp
// derivedLength expr 的加载期校验域 — 覆盖内建函数 hi8/lo8/hi16/lo16:
//   1) 函数名不是变量, 不得被判为"未声明输入变量";
//   2) 函数实参里的变量仍须通过声明域校验 (不能因加了函数就放行拼写错误);
//   3) 未知函数名在语法校验阶段即失败 (与运行时同一解析器)。

#include "MyProt/Service/ConfigValidator.hpp"
#include "MiniTest.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using MyProt::Service::ConfigValidator;
using MyProt::Service::ValidationResult;

/// 单个派生输出 X 的协议: inputs 声明 StartByteAddress / ByteCount,
/// 模板占位符 {AddrHi:X2} 供 derived 输出注入。
std::string ProtoJson(const std::string& expr) {
    return std::string(R"JSON({
  "schemaVersion": 2,
  "protocolName": "p-expr",
  "transport": { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 2,
    "lengthFieldLength": 2,
    "headerLength": 4,
    "maxFrameSize": 256
  },
  "inputs": {
    "StartByteAddress": { "source": "static", "value": 16, "label": "起始字节地址" },
    "ByteCount": { "source": "static", "value": 4, "label": "字节数" }
  },
  "operations": {
    "Read": {
      "kind": "read",
      "requestTemplate": [ "00 01", "{AddrHi:X2}", "{AddrLo:X2}", "{CntHi:X2}", "{CntLo:X2}" ],
      "outputs": {
        "AddrHi": { "source": "auto", "strategy": "derivedLength", "expr": ")JSON")
        + expr
        + R"JSON(" },
        "AddrLo": { "source": "auto", "strategy": "derivedLength", "expr": "lo8(StartByteAddress)" },
        "CntHi": { "source": "auto", "strategy": "derivedLength", "expr": "hi8(ByteCount)" },
        "CntLo": { "source": "auto", "strategy": "derivedLength", "expr": "lo8(ByteCount)" }
      }
    }
  },
  "handshake": []
})JSON";
}

ValidationResult Validate(const std::string& expr) {
    const MyProt::Core::Expected<ValidationResult> r =
        ConfigValidator::validateProtocolJson(ProtoJson(expr));
    // 解析器层的拒绝 (如 expr 语法非法) 以 Expected 错误返回, 同样计为一条校验错误
    if (!r.has_value()) {
        ValidationResult failed;
        failed.addError(r.error().message);
        return failed;
    }
    return r.value();
}

bool Contains(const std::vector<std::string>& list, const std::string& needle) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

void Dump(const char* title, const ValidationResult& r) {
    for (size_t i = 0; i < r.errors.size(); ++i)
        std::cout << "  [" << title << "][error] " << r.errors[i] << std::endl;
    for (size_t i = 0; i < r.warnings.size(); ++i)
        std::cout << "  [" << title << "][warn ] " << r.warnings[i] << std::endl;
}

} // namespace

// ── 内建函数本身不参与声明域校验 ──
TEST(DerivedExprTest, BuiltinCallsAreNotTreatedAsVariables) {
    const ValidationResult r = Validate("hi8(StartByteAddress / 2)");
    Dump("hi8", r);
    EXPECT_FALSE(r.hasErrors());
}

TEST(DerivedExprTest, EveryBuiltinIsAccepted) {
    const char* const exprs[] = {
        "lo8(StartByteAddress)",
        "hi16(StartByteAddress)",
        "lo16(StartByteAddress)",
        "lo8(hi16(StartByteAddress))",       // 嵌套
        "hi8 ( StartByteAddress )",           // 函数名与括号间空白
    };
    for (size_t i = 0; i < sizeof(exprs) / sizeof(exprs[0]); ++i) {
        const ValidationResult r = Validate(exprs[i]);
        Dump(exprs[i], r);
        EXPECT_FALSE(r.hasErrors());
    }
}

// ── 函数实参仍走声明域校验 ──
TEST(DerivedExprTest, UndeclaredVariableInsideCallIsError) {
    const ValidationResult r = Validate("hi8(StartByteAddr / 2)");   // 拼写错误
    Dump("undeclared", r);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_TRUE(Contains(r.errors, "未声明"));
    EXPECT_TRUE(Contains(r.errors, "StartByteAddr"));
}

// ── 未知函数名: 语法校验阶段即拒绝 (与运行时同一解析器) ──
TEST(DerivedExprTest, UnknownFunctionIsRejected) {
    const ValidationResult r = Validate("hi9(StartByteAddress)");
    Dump("unknown-func", r);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_TRUE(Contains(r.errors, "语法非法"));
}

// tests/Service.Tests/OperationProfileTests.cpp
// operationProfiles 解析期合并契约:
//   1) 原型 outputs/responseParser 作为操作默认值, 操作显式字段优先;
//   2) extends 单继承: 父原型先合并, 子原型覆盖; 禁止环;
//   3) 原型只承载 outputs(derivedLength) / responseParser, 其它字段一律拒绝;
//   4) 无 operationProfiles 的旧配置保持原行为 (向后兼容)。

#include "MyProt/Service/ConfigValidator.hpp"
#include "MiniTest.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using MyProt::Core::Expected;
using MyProt::Core::ProtocolConfig;
using MyProt::Service::ConfigValidator;
using MyProt::Service::ProtocolValidationOutcome;
using MyProt::Service::ValidationResult;

/// 最小可加载协议: 三个输入变量 + 可选 operationProfiles。
std::string ProtoJson(const std::string& profiles, const std::string& operations) {
    std::string j = R"JSON({
  "schemaVersion": 2,
  "protocolName": "p-profile",
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
    "ByteCount": { "source": "static", "value": 4, "label": "字节数" },
    "WriteValue": { "source": "static", "label": "写入值" }
  },
)JSON";
    if (!profiles.empty()) j += "\"operationProfiles\": " + profiles + ",\n";
    j += "\"operations\": " + operations + ",\n\"handshake\": []\n}";
    return j;
}

Expected<ProtocolValidationOutcome> Parse(const std::string& profiles,
                                         const std::string& operations) {
    return ConfigValidator::validateAndParseProtocolJson(ProtoJson(profiles, operations));
}

bool Contains(const std::vector<std::string>& list, const std::string& needle) {
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

void Dump(const char* title, const Expected<ProtocolValidationOutcome>& r) {
    if (!r.has_value()) {
        std::cout << "  [" << title << "][parse] " << r.error().message << std::endl;
        return;
    }
    const ValidationResult& v = r.value().result;
    for (size_t i = 0; i < v.errors.size(); ++i)
        std::cout << "  [" << title << "][error] " << v.errors[i] << std::endl;
    for (size_t i = 0; i < v.warnings.size(); ++i)
        std::cout << "  [" << title << "][warn ] " << v.warnings[i] << std::endl;
}

/// 解析器层拒绝以 Expected 错误返回; 校验层拒绝在 result.errors 中。
bool HasError(const Expected<ProtocolValidationOutcome>& r, const std::string& needle) {
    if (!r.has_value()) return r.error().message.find(needle) != std::string::npos;
    return Contains(r.value().result.errors, needle);
}

// ────────────────────────── 原语 ──────────────────────────

const char* const kReadOp =
    R"({"kind":"read","requestTemplate":["00 01","{StartAddress:X4}","{RegisterCount:X2}"],"profile":"readBits"})";

const char* const kReadProfiles = R"({
  "readResponse": {
    "responseParser": { "dataStartIndex": 9, "dataLengthExpr": "resp[8]" }
  },
  "readBits": {
    "extends": "readResponse",
    "description": "读位",
    "outputs": {
      "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress * 8" },
      "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "ByteCount * 8" }
    }
  }
})";

TEST(OperationProfileTest, MergesProfileOutputsAndParser) {
    const Expected<ProtocolValidationOutcome> r = Parse(kReadProfiles, std::string("{\"Read\":") + kReadOp + "}");
    Dump("merge", r);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value().result.hasErrors());
    const ProtocolConfig& p = r.value().proto;
    EXPECT_EQ(p.operations.size(), static_cast<size_t>(1));
    const MyProt::Core::OperationConfig& op = p.operations.at("Read");
    EXPECT_EQ(op.outputs.size(), static_cast<size_t>(2));
    EXPECT_EQ(op.outputs.at("StartAddress").expr, std::string("StartByteAddress * 8"));
    EXPECT_EQ(op.responseParser.dataStartIndex, 9);
    EXPECT_EQ(op.responseParser.dataLengthExpr, std::string("resp[8]"));
    EXPECT_EQ(op.responseParser.validCondition, std::string(""));
}

TEST(OperationProfileTest, OperationFieldsOverrideProfile) {
    const std::string op = R"({
      "kind": "read",
      "requestTemplate": ["00 01", "{StartAddress:X4}", "{RegisterCount:X2}"],
      "profile": "readBits",
      "responseParser": { "dataStartIndex": 11 },
      "outputs": {
        "RegisterCount": { "source": "auto", "strategy": "derivedLength", "expr": "ByteCount * 4" }
      }
    })";
    const Expected<ProtocolValidationOutcome> r = Parse(kReadProfiles, "{\"Read\":" + op + "}");
    Dump("override", r);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value().result.hasErrors());
    const MyProt::Core::OperationConfig& parsed = r.value().proto.operations.at("Read");
    // 操作显式字段胜出, 原型其余字段保留
    EXPECT_EQ(parsed.responseParser.dataStartIndex, 11);
    EXPECT_EQ(parsed.responseParser.dataLengthExpr, std::string("resp[8]"));
    EXPECT_EQ(parsed.outputs.at("RegisterCount").expr, std::string("ByteCount * 4"));
    EXPECT_EQ(parsed.outputs.at("StartAddress").expr, std::string("StartByteAddress * 8"));
}

TEST(OperationProfileTest, ExtendsMergesParentThenChild) {
    const std::string profiles = R"({
      "base": { "responseParser": { "dataStartIndex": 9, "dataLengthExpr": "resp[8]" } },
      "child": {
        "extends": "base",
        "responseParser": { "validCondition": "resp[7] == 0x03" },
        "outputs": {
          "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress / 2" }
        }
      }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01","{StartAddress:X4}"],"profile":"child"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("extends", r);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value().result.hasErrors());
    const MyProt::Core::OperationConfig& parsed = r.value().proto.operations.at("Read");
    EXPECT_EQ(parsed.responseParser.dataStartIndex, 9);
    EXPECT_EQ(parsed.responseParser.dataLengthExpr, std::string("resp[8]"));
    EXPECT_EQ(parsed.responseParser.validCondition, std::string("resp[7] == 0x03"));
    EXPECT_EQ(parsed.outputs.at("StartAddress").expr, std::string("StartByteAddress / 2"));
}

TEST(OperationProfileTest, CycleIsRejected) {
    const std::string profiles = R"({
      "a": { "extends": "b", "responseParser": { "dataStartIndex": 1 } },
      "b": { "extends": "a", "responseParser": { "dataStartIndex": 2 } }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"a"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("cycle", r);
    EXPECT_TRUE(HasError(r, "循环"));
}

TEST(OperationProfileTest, MissingProfileIsRejected) {
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"nope"})";
    const Expected<ProtocolValidationOutcome> r = Parse(kReadProfiles, "{\"Read\":" + op + "}");
    Dump("missing", r);
    EXPECT_TRUE(HasError(r, "未定义"));
}

TEST(OperationProfileTest, ExtendsUndefinedProfileIsRejected) {
    const std::string profiles = R"({
      "child": { "extends": "ghost", "responseParser": { "dataStartIndex": 9 } }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"child"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("extends-missing", r);
    EXPECT_TRUE(HasError(r, "未定义"));
}

TEST(OperationProfileTest, UnknownProfileFieldIsRejected) {
    const std::string profiles = R"({
      "p": { "kind": "read", "responseParser": { "dataStartIndex": 9 } }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"p"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("unknown-field", r);
    EXPECT_TRUE(HasError(r, "未知字段"));
}

TEST(OperationProfileTest, ProfileWithoutPayloadIsRejected) {
    const std::string profiles = R"({ "p": { "description": "空壳" } })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"p"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("no-payload", r);
    EXPECT_TRUE(HasError(r, "至少声明"));
}

TEST(OperationProfileTest, ProfileOutputsMustBeDerivedLength) {
    const std::string profiles = R"({
      "p": {
        "outputs": { "StartAddress": { "source": "static", "value": 1 } }
      }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"p"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("static-output", r);
    EXPECT_TRUE(HasError(r, "仅允许"));
}

TEST(OperationProfileTest, EmptyProfileNameOnOperationIsRejected) {
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":""})";
    const Expected<ProtocolValidationOutcome> r = Parse(kReadProfiles, "{\"Read\":" + op + "}");
    Dump("empty-name", r);
    EXPECT_TRUE(HasError(r, "不能为空"));
}

TEST(OperationProfileTest, LegacyConfigWithoutProfilesStillParses) {
    const std::string op = R"({
      "kind": "read",
      "requestTemplate": ["00 01", "{StartAddress:X4}"],
      "responseParser": { "validCondition": "resp[7] == 0x03", "dataStartIndex": 9 },
      "outputs": {
        "StartAddress": { "source": "auto", "strategy": "derivedLength", "expr": "StartByteAddress / 2" }
      }
    })";
    const Expected<ProtocolValidationOutcome> r = Parse("", "{\"Read\":" + op + "}");
    Dump("legacy", r);
    ASSERT_TRUE(r.has_value());
    EXPECT_FALSE(r.value().result.hasErrors());
    const MyProt::Core::OperationConfig& parsed = r.value().proto.operations.at("Read");
    EXPECT_EQ(parsed.outputs.size(), static_cast<size_t>(1));
    EXPECT_EQ(parsed.responseParser.validCondition, std::string("resp[7] == 0x03"));
}

TEST(OperationProfileTest, InvalidValidConditionInProfileIsRejected) {
    const std::string profiles = R"({
      "p": { "responseParser": { "validCondition": "resp[7] ==" } }
    })";
    const std::string op = R"({"kind":"read","requestTemplate":["00 01"],"profile":"p"})";
    const Expected<ProtocolValidationOutcome> r = Parse(profiles, "{\"Read\":" + op + "}");
    Dump("bad-condition", r);
    EXPECT_TRUE(HasError(r, "语法错误"));
}

} // namespace

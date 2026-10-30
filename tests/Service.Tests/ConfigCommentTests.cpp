// tests/Service.Tests/ConfigCommentTests.cpp
// 配置文件 JSON 的注释策略 (ConfigJsonText.hpp):
//   1) // 与 /* */ 对校验/解析是语义透明的 (与去注释版本得到同一份错误清单);
//   2) 字符串字面量里的 "//"、"/*" 不得被当作注释截断;
//   3) UTF-8 BOM 可容忍 (Windows 编辑器 "UTF-8 with BOM" 另存);
//   4) 未闭合的块注释仍然是语法错误 (不能放松到吞掉半份配置);
//   5) tags.json 根文本同样允许注释。
//
// 注释只由 nlohmann 的 ignore_comments 在解析期丢弃, 不进入 POCO:
// 因此热重载 / 渲染 / --validate 的行为与无注释配置完全一致。

#include "MyProt/Service/ConfigValidator.hpp"
#include "MiniTest.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace {

using MyProt::Service::ConfigValidator;
using MyProt::Service::ValidationResult;

/// 一份能通过加载期校验的最小 LengthField 协议 (与 DerivedExprTests 的正例同构)。
/// withComments = false 的版本用于比对错误清单, 证明注释不改变语义。
std::string ProtoJson(bool withComments) {
    const std::string lineComment = withComments ? "  // 帧头: 事务号 + 长度域\n" : "";
    const std::string blockComment =
        withComments ? "  /* 站号/数量按设备手册填写 */\n" : "";
    return std::string("{\n") + lineComment
        + "  \"schemaVersion\": 2,\n"
        + "  \"protocolName\": \"p-comments\",\n"
        + blockComment
        + "  \"transport\": { \"type\": \"Tcp\", \"defaultPort\": 502 },\n"
        + "  \"framing\": {\n"
        + "    \"type\": \"LengthField\",\n"
        + "    \"lengthFieldOffset\": 2,\n"
        + "    \"lengthFieldLength\": 2,\n"
        + "    \"headerLength\": 4,\n"
        + "    \"maxFrameSize\": 256\n"
        + "  },\n"
        + "  \"inputs\": {\n"
        // 字符串里的注释符号必须原样保留
        + "    \"StartByteAddress\": { \"source\": \"static\", \"value\": 16,\n"
        + "      \"label\": \"起始地址 // 不是注释 /* 也不是 */\" },\n"
        + "    \"ByteCount\": { \"source\": \"static\", \"value\": 4 }\n"
        + "  },\n"
        + "  \"operations\": {\n"
        + "    \"Read\": {\n"
        + "      \"kind\": \"read\",\n"
        + "      \"requestTemplate\": [ \"00 01\", \"{AddrHi:X2}\", \"{AddrLo:X2}\",\n"
        + "                            \"{CntHi:X2}\", \"{CntLo:X2}\" ],\n"
        + "      \"outputs\": {\n"
        + "        \"AddrHi\": { \"source\": \"auto\", \"strategy\": \"derivedLength\", "
          "\"expr\": \"hi8(StartByteAddress)\" },\n"
        + "        \"AddrLo\": { \"source\": \"auto\", \"strategy\": \"derivedLength\", "
          "\"expr\": \"lo8(StartByteAddress)\" },\n"
        + "        \"CntHi\": { \"source\": \"auto\", \"strategy\": \"derivedLength\", "
          "\"expr\": \"hi8(ByteCount)\" },\n"
        + "        \"CntLo\": { \"source\": \"auto\", \"strategy\": \"derivedLength\", "
          "\"expr\": \"lo8(ByteCount)\" }\n"
        + "      }\n"
        + "    }\n"
        + "  },\n"
        + "  \"handshake\": []\n"
        + "}\n";
}

ValidationResult ValidateOrFail(const std::string& text, bool& ok) {
    const MyProt::Core::Expected<ValidationResult> r =
        ConfigValidator::validateProtocolJson(text);
    ok = r.has_value();
    if (!ok) {
        ValidationResult failed;
        failed.addError(r.error().message);
        return failed;
    }
    return r.value();
}

} // namespace

// 注释对校验结果完全透明: 带注释版本 = 去注释版本 (逐条错误文本一致)
TEST(ConfigCommentTest, CommentsAreSemanticallyTransparent) {
    bool okPlain = false, okCommented = false;
    const ValidationResult plain = ValidateOrFail(ProtoJson(false), okPlain);
    const ValidationResult commented = ValidateOrFail(ProtoJson(true), okCommented);

    EXPECT_TRUE(okPlain);
    EXPECT_TRUE(okCommented);
    EXPECT_EQ(commented.errors.size(), plain.errors.size());
    EXPECT_EQ(commented.errors.size(), static_cast<size_t>(0));
    EXPECT_EQ(commented.warnings.size(), plain.warnings.size());
}

// 字符串字面量里的注释符号不得被截断 (否则 label 会被吃掉)
TEST(ConfigCommentTest, CommentMarkersInsideStringsArePreserved) {
    const MyProt::Core::Expected<MyProt::Core::ProtocolConfig> p =
        ConfigValidator::parseProtocolJson(ProtoJson(true));
    ASSERT_TRUE(p.has_value());

    const std::unordered_map<std::string, MyProt::Core::VariableConfig>::const_iterator
        it = p.value().inputs.find("StartByteAddress");
    ASSERT_TRUE(it != p.value().inputs.end());
    EXPECT_TRUE(it->second.label.find("// 不是注释") != std::string::npos);
    EXPECT_TRUE(it->second.label.find("/* 也不是 */") != std::string::npos);
}

// UTF-8 BOM: 首三字节被跳过, 结果与非 BOM 版本一致
TEST(ConfigCommentTest, Utf8BomIsTolerated) {
    const std::string bom("\xEF\xBB\xBF");
    bool okPlain = false, okBom = false;
    const ValidationResult plain = ValidateOrFail(ProtoJson(false), okPlain);
    const ValidationResult withBom = ValidateOrFail(bom + ProtoJson(false), okBom);

    EXPECT_TRUE(okPlain);
    EXPECT_TRUE(okBom);
    EXPECT_EQ(withBom.errors.size(), plain.errors.size());
}

// 未闭合的块注释仍是语法错误 (Expected 失败, 而不是被静默吞掉)
TEST(ConfigCommentTest, UnterminatedBlockCommentStillFails) {
    const MyProt::Core::Expected<ValidationResult> r =
        ConfigValidator::validateProtocolJson("{ /* 没有收尾\n \"protocolName\": \"x\" }");
    EXPECT_FALSE(r.has_value());
    if (!r.has_value()) {
        EXPECT_TRUE(r.error().message.find("JSON 语法错误") != std::string::npos);
    }
}

// tags.json 根文本同样允许注释
TEST(ConfigCommentTest, ConfigRootCommentsAccepted) {
    const std::string root =
        "{\n"
        "  // 设备清单\n"
        "  \"schemaVersion\": 2,\n"
        "  \"devices\": [],\n"
        "  /* 标签清单 (暂无) */\n"
        "  \"tags\": []\n"
        "}\n";
    const MyProt::Core::Expected<ValidationResult> r =
        ConfigValidator::validateConfigRootJson(root);
    EXPECT_TRUE(r.has_value());
}

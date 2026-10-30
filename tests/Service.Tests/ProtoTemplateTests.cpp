// tests/Service.Tests/ProtoTemplateTests.cpp
// P0/P1 护栏:
//   1) `--proto-init` 的三份模板必须能被真实校验器与解析器接受
//      (否则用户拿到的"第一份配置"就是坏的);
//   2) devices[].variables 的作用域校验: 未知键报错 / auto 键告警 / 保留名报错;
//   3) 解析期设备级回退合并: 设备键进入 tag.variables, 标签同名键优先。

#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Service/ProtoTemplate.hpp"
#include "MiniTest.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

using MyProt::Service::ConfigValidator;
using MyProt::Service::ProtoTemplate;
using MyProt::Service::ValidationResult;

/// 最小协议: static UnitID + autoIncrement Seq + 一个读操作。
std::string ProtoJson() {
    return R"JSON({
  "schemaVersion": 2,
  "protocolName": "p-test",
  "transport": { "type": "Tcp", "defaultPort": 502 },
  "framing": {
    "type": "LengthField",
    "lengthFieldOffset": 2,
    "lengthFieldLength": 2,
    "headerLength": 4,
    "maxFrameSize": 256
  },
  "inputs": {
    "UnitID": { "source": "static", "value": 1, "label": "站号" },
    "StartByteAddress": { "source": "static", "value": 0, "label": "起始字节地址" },
    "ByteCount": { "source": "static", "value": 2, "label": "数据字节跨度" },
    "Seq": { "source": "auto", "strategy": "autoIncrement", "params": { "seed": 0 } }
  },
  "operations": {
    "Read": {
      "kind": "read",
      "requestTemplate": [ "00 01", "{UnitID:X2}", "{Seq:X2}" ]
    }
  },
  "handshake": []
})JSON";
}

/// 设备层变量片段 -> 完整 tags.json 文本
std::string RootJson(const std::string& deviceVariables,
                     const std::string& tagVariables) {
    return std::string(R"JSON({
  "schemaVersion": 2,
  "devices": [
    {
      "id": "D1",
      "protocol": "p-test",
      "connection": { "host": "127.0.0.1", "port": 502, "timeoutMs": 1000 },
      "requestTimeoutMs": 1000,
      "variables": )JSON")
        + deviceVariables
        + R"JSON(
    }
  ],
  "tags": [
    {
      "name": "T1",
      "deviceId": "D1",
      "operation": "Read",
      "finalType": "UInt16",
      "variables": )JSON"
        + tagVariables
        + R"JSON(
    }
  ]
})JSON";
}

ValidationResult ValidateRoot(const std::string& deviceVariables,
                              const std::string& tagVariables) {
    const std::vector<std::string> protos(1, ProtoJson());
    const MyProt::Core::Expected<ValidationResult> r =
        ConfigValidator::validateConfigRootJson(
            RootJson(deviceVariables, tagVariables), protos);
    if (!r.has_value()) {
        ValidationResult failed;
        failed.addError("validateConfigRootJson 调用失败");
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

// ── P0: 脚手架模板可用性 ──

TEST(ProtoTemplateTest, ListIsStable) {
    const std::vector<ProtoTemplate>& all = MyProt::Service::ListProtoTemplates();
    EXPECT_EQ(all.size(), static_cast<size_t>(3));
    EXPECT_TRUE(MyProt::Service::FindProtoTemplate("lengthfield") != 0);
    EXPECT_TRUE(MyProt::Service::FindProtoTemplate("fixed") != 0);
    EXPECT_TRUE(MyProt::Service::FindProtoTemplate("serial") != 0);
    EXPECT_TRUE(MyProt::Service::FindProtoTemplate("nope") == 0);
}

TEST(ProtoTemplateTest, EveryTemplatePassesValidatorAndParser) {
    const std::vector<ProtoTemplate>& all = MyProt::Service::ListProtoTemplates();
    for (size_t i = 0; i < all.size(); ++i) {
        const std::string& text = all[i].json;
        const MyProt::Core::Expected<ValidationResult> vr =
            ConfigValidator::validateProtocolJson(text);
        bool ok = vr.has_value() && !vr.value().hasErrors();
        if (vr.has_value() && vr.value().hasErrors()) {
            Dump(all[i].kind.c_str(), vr.value());
        }
        EXPECT_TRUE(ok);
        if (!ok) continue;

        const MyProt::Core::Expected<MyProt::Core::ProtocolConfig> pr =
            ConfigValidator::parseProtocolJson(text);
        EXPECT_TRUE(pr.has_value());
        if (pr.has_value()) {
            EXPECT_EQ(pr.value().protocolName, all[i].protocolName);
            EXPECT_TRUE(!pr.value().operations.empty());
        }
    }
}

// ── P1: 设备级变量作用域校验 ──

TEST(DeviceVariablesTest, KnownStaticVariablePasses) {
    const ValidationResult r = ValidateRoot("{\"UnitID\": 3}", "{}");
    Dump("known", r);
    EXPECT_FALSE(r.hasErrors());
}

TEST(DeviceVariablesTest, UnknownVariableIsError) {
    // 典型拼写错误: DA1 -> DA10 (未声明的键会在运行期被静默忽略)
    const ValidationResult r = ValidateRoot("{\"UnitId\": 3}", "{}");
    Dump("unknown", r);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_TRUE(Contains(r.errors, "未声明变量"));
    EXPECT_TRUE(Contains(r.errors, "UnitId"));
}

TEST(DeviceVariablesTest, AutoVariableIsWarningOnly) {
    const ValidationResult r = ValidateRoot("{\"Seq\": 5}", "{}");
    Dump("auto", r);
    EXPECT_FALSE(r.hasErrors());
    EXPECT_TRUE(Contains(r.warnings, "auto"));
}

TEST(DeviceVariablesTest, ReservedNameIsError) {
    const ValidationResult r = ValidateRoot("{\"Frame\": 1}", "{}");
    Dump("reserved", r);
    EXPECT_TRUE(r.hasErrors());
    EXPECT_TRUE(Contains(r.errors, "保留名"));
}

// ── P1: 设备级回退合并 (解析期) ──

TEST(DeviceVariablesTest, DeviceValuesFlowIntoTagsAndTagWins) {
    const MyProt::Core::Expected<MyProt::Core::ProtocolConfig> parsed =
        ConfigValidator::parseProtocolJson(ProtoJson());
    EXPECT_TRUE(parsed.has_value());
    if (!parsed.has_value()) return;
    const std::vector<MyProt::Core::ProtocolConfig> protos(1, parsed.value());
    const MyProt::Core::Expected<MyProt::Service::ConfigRootValidationOutcome> out =
        ConfigValidator::validateAndParseConfigRootJson(
            RootJson("{\"UnitID\": 7, \"StartByteAddress\": 10}",
                     "{\"StartByteAddress\": 0, \"ByteCount\": 2}"),
            protos);
    EXPECT_TRUE(out.has_value());
    if (!out.has_value()) return;
    Dump("merge", out.value().result);
    EXPECT_FALSE(out.value().result.hasErrors());
    EXPECT_EQ(out.value().root.tags.size(), static_cast<size_t>(1));
    if (out.value().root.tags.empty()) return;

    const MyProt::Core::TagDefinition& tag = out.value().root.tags[0];
    // 设备键被继承 ...
    EXPECT_EQ(tag.variables.count("UnitID"), static_cast<size_t>(1));
    EXPECT_EQ(tag.variables.find("UnitID")->second, static_cast<uint32_t>(7));
    // ... 标签同名键优先
    EXPECT_EQ(tag.variables.find("StartByteAddress")->second, static_cast<uint32_t>(0));
}

// tests/Service.Tests/TrialPreviewTests.cpp
// 试渲染预览 (Service::PreviewProtocolOperation):
//   1) 帧字节 = 网关同一渲染管线的产物 (模板 + 派生长度 + 成帧槽位);
//   2) 覆盖变量生效且来源可追踪 (protocol / operation / override / derived / auto);
//   3) 帧长槽位不一致必须报错, 而不是给出一帧看似正常的字节;
//   4) 候选文本的注释 / BOM 与 --validate 同策略;
//   5) {Name:raw} 载荷参与派生长度计算;
//   6) 操作不存在 / JSON 语法错误 → ok=false 且原因可读。

#include "MyProt/Service/TrialPreview.hpp"
#include "MiniTest.hpp"

#include <string>
#include <vector>

namespace {

using MyProt::Service::TrialPreviewRequest;
using MyProt::Service::TrialPreviewResult;

/// 5 字节头 + 2 字节地址的最小协议: 共 10 字节, 长度槽位 = 总长 - headerLength(4)。
/// expr 用 {Frame:fixed} - 4 ({Frame:fixed} = 整个固定段 10B, headerLength = 4)。
std::string ScalarProtocol(const std::string& expr = "{Frame:fixed} - 4") {
    return std::string("{\n")
        + "  \"schemaVersion\": 2,\n"
        + "  \"protocolName\": \"p-preview\",\n"
        + "  \"transport\": { \"type\": \"Tcp\", \"defaultPort\": 9000 },\n"
        + "  \"framing\": {\n"
        + "    \"type\": \"LengthField\",\n"
        + "    \"lengthFieldOffset\": 2,\n"
        + "    \"lengthFieldLength\": 2,\n"
        + "    \"lengthIncludesHeader\": false,\n"
        + "    \"byteOrder\": \"BigEndian\",\n"
        + "    \"headerLength\": 4,\n"
        + "    \"maxFrameSize\": 256\n"
        + "  },\n"
        + "  \"inputs\": { \"UnitID\": { \"source\": \"static\", \"value\": 1 } },\n"
        + "  \"operations\": {\n"
        + "    \"Read\": {\n"
        + "      \"kind\": \"read\",\n"
        + "      \"requestTemplate\": [ \"AA BB\", \"{Len:X4}\", \"{UnitID:X4}\",\n"
        + "                            \"{AddrHi:X4}\", \"{AddrLo:X4}\" ],\n"
        + "      \"inputs\": {\n"
        + "        \"AddrHi\": { \"source\": \"static\", \"value\": 0 },\n"
        + "        \"AddrLo\": { \"source\": \"static\", \"value\": 100 }\n"
        + "      },\n"
        + "      \"outputs\": {\n"
        + "        \"Len\": { \"source\": \"auto\", \"strategy\": \"derivedLength\", \"expr\": \""
        + expr + "\" }\n"
        + "      },\n"
        + "      \"responseParser\": { \"validCondition\": \"resp[0] == 0\",\n"
        + "                          \"dataStartIndex\": 1, \"dataLengthExpr\": \"\" }\n"
        + "    }\n"
        + "  },\n"
        + "  \"handshake\": []\n"
        + "}\n";
}

/// 变长载荷协议: {Data:raw} 的字节数参与派生长度 (expr = {Data:len})。
std::string RawProtocol() {
    return std::string("{\n")
        + "  \"schemaVersion\": 2,\n"
        + "  \"protocolName\": \"p-preview-raw\",\n"
        + "  \"transport\": { \"type\": \"Tcp\", \"defaultPort\": 9000 },\n"
        + "  \"framing\": {\n"
        + "    \"type\": \"LengthField\",\n"
        + "    \"lengthFieldOffset\": 2,\n"
        + "    \"lengthFieldLength\": 2,\n"
        + "    \"lengthIncludesHeader\": false,\n"
        + "    \"byteOrder\": \"BigEndian\",\n"
        + "    \"headerLength\": 4,\n"
        + "    \"maxFrameSize\": 256\n"
        + "  },\n"
        + "  \"operations\": {\n"
        + "    \"WriteRaw\": {\n"
        + "      \"kind\": \"write\",\n"
        + "      \"requestTemplate\": [ \"AA BB\", \"{Len:X4}\", \"{Data:raw}\" ],\n"
        + "      \"outputs\": {\n"
        + "        \"Len\": { \"source\": \"auto\", \"strategy\": \"derivedLength\",\n"
        + "                 \"expr\": \"{Data:len}\" }\n"
        + "      },\n"
        + "      \"responseParser\": { \"validCondition\": \"resp[0] == 0\",\n"
        + "                          \"dataStartIndex\": 1, \"dataLengthExpr\": \"\" }\n"
        + "    }\n"
        + "  },\n"
        + "  \"handshake\": []\n"
        + "}\n";
}

/// 自动增量变量协议: TID 由 AutoComputeProvider 在渲染期求值。
std::string AutoProtocol() {
    return std::string("{\n")
        + "  \"schemaVersion\": 2,\n"
        + "  \"protocolName\": \"p-preview-auto\",\n"
        + "  \"transport\": { \"type\": \"Tcp\", \"defaultPort\": 9000 },\n"
        + "  \"framing\": {\n"
        + "    \"type\": \"LengthField\",\n"
        + "    \"lengthFieldOffset\": 2,\n"
        + "    \"lengthFieldLength\": 2,\n"
        + "    \"lengthIncludesHeader\": false,\n"
        + "    \"byteOrder\": \"BigEndian\",\n"
        + "    \"headerLength\": 4,\n"
        + "    \"maxFrameSize\": 256\n"
        + "  },\n"
        + "  \"inputs\": {\n"
        + "    \"TID\": { \"source\": \"auto\", \"strategy\": \"autoIncrement\",\n"
        + "             \"params\": { \"seed\": 1 } }\n"
        + "  },\n"
        + "  \"operations\": {\n"
        + "    \"Read\": {\n"
        + "      \"kind\": \"read\",\n"
        + "      \"requestTemplate\": [ \"AA BB\", \"{Len:X4}\", \"{TID:X4}\" ],\n"
        + "      \"outputs\": {\n"
        + "        \"Len\": { \"source\": \"auto\", \"strategy\": \"derivedLength\",\n"
        + "                 \"expr\": \"{Frame:fixed} - 4\" }\n"
        + "      },\n"
        + "      \"responseParser\": { \"validCondition\": \"resp[0] == 0\",\n"
        + "                          \"dataStartIndex\": 1, \"dataLengthExpr\": \"\" }\n"
        + "    }\n"
        + "  },\n"
        + "  \"handshake\": []\n"
        + "}\n";
}

const MyProt::Service::TrialPreviewVariable* FindVar(
    const TrialPreviewResult& r, const std::string& name) {
    for (std::size_t i = 0; i < r.variables.size(); ++i) {
        if (r.variables[i].name == name) return &r.variables[i];
    }
    return 0;
}

bool HasErrorContaining(const TrialPreviewResult& r, const std::string& needle) {
    for (std::size_t i = 0; i < r.errors.size(); ++i) {
        if (r.errors[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

// 基础渲染: 字节与手工计算一致, 长度槽位自洽
TEST(TrialPreviewTest, RendersScalarOperation) {
    TrialPreviewRequest req;
    req.protocolJson = ScalarProtocol();
    req.operation = "Read";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameLength, static_cast<size_t>(10));
    EXPECT_EQ(r.frameHex, std::string("AA BB 00 06 00 01 00 00 00 64"));
    EXPECT_TRUE(r.hasLengthField);
    EXPECT_TRUE(r.lengthFieldOk);
    EXPECT_EQ(r.lengthFieldValue, static_cast<long long>(6));
    EXPECT_EQ(r.lengthFieldExpected, static_cast<long long>(6));
    EXPECT_EQ(r.lengthFieldByteOrder, std::string("BigEndian"));

    const MyProt::Service::TrialPreviewVariable* unit = FindVar(r, "UnitID");
    ASSERT_TRUE(unit != 0);
    EXPECT_EQ(unit->source, std::string("protocol"));
    EXPECT_EQ(unit->value, static_cast<unsigned long long>(1));

    const MyProt::Service::TrialPreviewVariable* len = FindVar(r, "Len");
    ASSERT_TRUE(len != 0);
    EXPECT_EQ(len->source, std::string("derived"));
    EXPECT_TRUE(len->hasValue);
    EXPECT_EQ(len->value, static_cast<unsigned long long>(6));
}

// 覆盖变量: 来源标记为 override, 且确实进入帧字节
TEST(TrialPreviewTest, OverrideVariablesReachTheFrame) {
    TrialPreviewRequest req;
    req.protocolJson = ScalarProtocol();
    req.operation = "Read";
    req.variables["UnitID"] = 7;
    req.variables["AddrLo"] = 200;

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameHex, std::string("AA BB 00 06 00 07 00 00 00 C8"));

    const MyProt::Service::TrialPreviewVariable* unit = FindVar(r, "UnitID");
    ASSERT_TRUE(unit != 0);
    EXPECT_EQ(unit->source, std::string("override"));
    EXPECT_EQ(unit->value, static_cast<unsigned long long>(7));
}

// 帧长槽位不一致: 必须 ok=false, 并在错误里点名"帧长一致性"
TEST(TrialPreviewTest, LengthMismatchIsReported) {
    TrialPreviewRequest req;
    req.protocolJson = ScalarProtocol("{Frame:fixed} - 3");   // 长度槽位少写 1
    req.operation = "Read";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.lengthFieldOk);
    EXPECT_TRUE(HasErrorContaining(r, "帧长一致性"));
}

// 候选文本允许注释 / BOM, 且渲染结果与无注释版一致
TEST(TrialPreviewTest, CommentsAndBomAreTolerated) {
    std::string text = ScalarProtocol();
    text = "// 顶部注释\n" + text;
    const std::string bom("\xEF\xBB\xBF");
    text = bom + text;

    TrialPreviewRequest req;
    req.protocolJson = text;
    req.operation = "Read";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameHex, std::string("AA BB 00 06 00 01 00 00 00 64"));
}

// {Name:raw} 载荷: 显式载荷决定派生长度; 未提供时使用占位并在 warnings 说明
TEST(TrialPreviewTest, RawPayloadDrivesDerivedLength) {
    TrialPreviewRequest req;
    req.protocolJson = RawProtocol();
    req.operation = "WriteRaw";
    req.rawVariables["Data"] = "01 02 03 04";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameLength, static_cast<size_t>(8));
    EXPECT_EQ(r.frameHex, std::string("AA BB 00 04 01 02 03 04"));
    EXPECT_TRUE(r.lengthFieldOk);

    TrialPreviewRequest dummy;
    dummy.protocolJson = RawProtocol();
    dummy.operation = "WriteRaw";
    dummy.payloadBytes = 2;
    const TrialPreviewResult r2 = MyProt::Service::PreviewProtocolOperation(dummy);
    EXPECT_TRUE(r2.ok);
    EXPECT_EQ(r2.frameHex, std::string("AA BB 00 02 01 02"));
    bool sawDummyWarning = false;
    for (std::size_t i = 0; i < r2.warnings.size(); ++i) {
        if (r2.warnings[i].find("占位") != std::string::npos) sawDummyWarning = true;
    }
    EXPECT_TRUE(sawDummyWarning);
}

// autoCompute 变量: 渲染期求值, 只报告来源不报告值
TEST(TrialPreviewTest, AutoVariablesAreReportedWithoutValue) {
    TrialPreviewRequest req;
    req.protocolJson = AutoProtocol();
    req.operation = "Read";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameHex, std::string("AA BB 00 02 00 01"));
    const MyProt::Service::TrialPreviewVariable* tid = FindVar(r, "TID");
    ASSERT_TRUE(tid != 0);
    EXPECT_EQ(tid->source, std::string("auto"));
    EXPECT_FALSE(tid->hasValue);
}

// 操作不存在: 错误里带可用操作清单
TEST(TrialPreviewTest, UnknownOperationListsAvailableOnes) {
    TrialPreviewRequest req;
    req.protocolJson = ScalarProtocol();
    req.operation = "Nope";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(HasErrorContaining(r, "不存在"));
    EXPECT_EQ(r.operations.size(), static_cast<size_t>(1));
    EXPECT_EQ(r.operations[0], std::string("Read"));
}

// 语法错误: ok=false 且不产出任何帧字节 (避免误导)
TEST(TrialPreviewTest, InvalidJsonFailsWithoutFrame) {
    TrialPreviewRequest req;
    req.protocolJson = "{ not json";
    req.operation = "Read";

    const TrialPreviewResult r = MyProt::Service::PreviewProtocolOperation(req);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.errors.empty());
    EXPECT_EQ(r.frameLength, static_cast<size_t>(0));
    EXPECT_TRUE(r.frameHex.empty());
}

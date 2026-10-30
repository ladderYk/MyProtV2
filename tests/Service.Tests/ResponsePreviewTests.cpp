// tests/Service.Tests/ResponsePreviewTests.cpp
// 响应解析预览 (Service::PreviewProtocolResponse):
//   1) 走网关同一 ResponseParser: finalType / dataStartIndex / 字节序语义逐位一致;
//   2) validCondition 不满足必须报错, 而不是给一个看似正常的值;
//   3) dataStartIndex 越界 / hex 非法 / 操作不存在 → ok=false 且原因可读;
//   4) ByteCount 超出数据区时退化为剩余全量并给出警告 (与运行时一致);
//   5) Bool + bitOffset 位提取; 未指定 operation 只返回操作清单.
//   6) 标签级 converters (scale k/b) 按序应用, 与网关一致; 换算前值单独回报.

#include "MyProt/Service/ResponsePreview.hpp"
#include "MiniTest.hpp"

#include <string>

namespace {

using MyProt::Service::ResponsePreviewRequest;
using MyProt::Service::ResponsePreviewResult;

/// 最小可校验协议: LengthField 成帧 + 一个读操作.
/// 响应约定: resp[1]==4 才有效, 数据区从第 2 字节开始.
std::string Proto() {
    return std::string("{\n")
        + "  \"schemaVersion\": 2,\n"
        + "  \"protocolName\": \"p-resp-preview\",\n"
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
        + "        \"Len\": { \"source\": \"auto\", \"strategy\": \"derivedLength\",\n"
        + "                 \"expr\": \"{Frame:fixed} - 4\" }\n"
        + "      },\n"
        + "      \"responseParser\": { \"validCondition\": \"resp[1] == 4\",\n"
        + "                          \"dataStartIndex\": 2, \"dataLengthExpr\": \"\" }\n"
        + "    }\n"
        + "  },\n"
        + "  \"handshake\": []\n"
        + "}\n";
}

ResponsePreviewRequest MakeRequest(const std::string& hex) {
    ResponsePreviewRequest r;
    r.protocolJson = Proto();
    r.operation = "Read";
    r.hex = hex;
    r.byteCount = 2;
    return r;
}

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST(ResponsePreviewTest, ParsesUInt16FromDataRegion) {
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(
        MakeRequest("03 04 00 2A 00 01"));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.frameLength, 6u);
    EXPECT_EQ(r.dataStartIndex, 2);
    EXPECT_EQ(r.rawLength, 2u);
    EXPECT_EQ(r.rawHex, std::string("00 2A"));
    EXPECT_EQ(r.valueType, std::string("UInt16"));
    EXPECT_EQ(r.valueText, std::string("42"));
    EXPECT_TRUE(r.hasNumeric);
    EXPECT_EQ(r.numeric, 42.0);
    EXPECT_EQ(r.quality, std::string("Good"));
    EXPECT_TRUE(r.conditionChecked);
    EXPECT_TRUE(r.conditionOk);
}

TEST(ResponsePreviewTest, ConditionFailureIsReported) {
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(
        MakeRequest("03 05 00 2A"));
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.conditionOk);
    EXPECT_TRUE(r.errors.size() >= 1);
    EXPECT_TRUE(Contains(r.errors[0], "validCondition"));
}

TEST(ResponsePreviewTest, DataStartIndexOutOfRange) {
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(
        MakeRequest("03"));                       // 1 字节 < dataStartIndex(2)
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.errors[0], "dataStartIndex"));
}

TEST(ResponsePreviewTest, EmptyDataRegionIsReportedAsParseFailure) {
    // dataStartIndex 恰好等于帧长: 索引本身合法, 但数据区为空 → 类型转换失败
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(
        MakeRequest("03 04"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.errors[0], "解析失败"));
}

TEST(ResponsePreviewTest, InvalidHexIsRejected) {
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(
        MakeRequest("0G 01"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.errors[0], "hex"));
}

TEST(ResponsePreviewTest, ByteCountBeyondDataFallsBackWithWarning) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");   // 数据区只有 2 字节
    req.byteCount = 8;
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.rawLength, 2u);
    EXPECT_EQ(r.valueText, std::string("42"));
    bool warned = false;
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        if (Contains(r.warnings[i], "ByteCount")) warned = true;
    }
    EXPECT_TRUE(warned);
}

TEST(ResponsePreviewTest, ByteOrderOverrideIsApplied) {
    ResponsePreviewRequest req = MakeRequest("03 04 2A 00");
    req.byteOrder = "LittleEndian";
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.byteOrder, std::string("LittleEndian"));
    EXPECT_EQ(r.valueText, std::string("42"));
}

TEST(ResponsePreviewTest, BoolBitExtraction) {
    ResponsePreviewRequest req = MakeRequest("03 04 08 00");
    req.finalType = "Bool";
    req.bitOffset = 3;
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.valueType, std::string("Bool"));
    EXPECT_EQ(r.valueText, std::string("true"));
    EXPECT_FALSE(r.hasNumeric);
}

TEST(ResponsePreviewTest, UnknownOperationListsAvailable) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");
    req.operation = "Nope";
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.operations.size(), 1u);
    EXPECT_TRUE(Contains(r.errors[0], "Read"));
}

TEST(ResponsePreviewTest, MissingOperationReturnsListOnly) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");
    req.operation = "";
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.operations.size(), 1u);
    EXPECT_FALSE(r.hasValue);
}

// ── 标签级换算链 (v5 C1): 预览值与网关 ResponseParser 逐位一致 ──

TEST(ResponsePreviewTest, ConverterChainProducesEngineeringValue) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");   // 原始值 42
    MyProt::Core::ScaleConverter c;
    c.k = 0.1;
    c.b = -40.0;                                                // 42*0.1-40 = -35.8
    req.converters.push_back(c);
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.valueType, std::string("Double"));
    EXPECT_EQ(r.valueText, std::string("-35.8"));
    EXPECT_EQ(r.converters.size(), 1u);
    EXPECT_TRUE(r.hasPreValue);                                 // 换算前值用于 UI 对照
    EXPECT_EQ(r.preValueType, std::string("UInt16"));
    EXPECT_EQ(r.preValueText, std::string("42"));
}

TEST(ResponsePreviewTest, ConverterChainAppliesInOrder) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");   // 42 -> *2 -> +1 = 85
    MyProt::Core::ScaleConverter c1;
    c1.k = 2.0; c1.b = 0.0;
    MyProt::Core::ScaleConverter c2;
    c2.k = 1.0; c2.b = 1.0;
    req.converters.push_back(c1);
    req.converters.push_back(c2);
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.valueText, std::string("85"));
    EXPECT_EQ(r.converters.size(), 2u);
}

TEST(ResponsePreviewTest, ConvertersRejectedForNonNumericFinalType) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 01");
    req.finalType = "Bool";                                     // 规则13: 仅数值型可换算
    MyProt::Core::ScaleConverter c;
    c.k = 1.0; c.b = 0.0;
    req.converters.push_back(c);
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.errors[0], "converters"));
}

TEST(ResponsePreviewTest, ZeroScaleIsRejected) {
    ResponsePreviewRequest req = MakeRequest("03 04 00 2A");
    MyProt::Core::ScaleConverter c;
    c.k = 0.0; c.b = 1.0;                                       // 值域塌缩
    req.converters.push_back(c);
    const ResponsePreviewResult r = MyProt::Service::PreviewProtocolResponse(req);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(Contains(r.errors[0], "不可为 0"));
}


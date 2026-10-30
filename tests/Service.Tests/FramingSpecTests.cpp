// tests/Service.Tests/FramingSpecTests.cpp
// Validation of the config-driven framing primitives (ADR-0021):
// varint length field, bit fields, dispatch and repeated records.
//
// The assertions look only for the specific framing messages under test; a minimal
// protocol JSON intentionally triggers unrelated rule-6 errors (empty operations), and
// those must not make these cases flaky.

#include "MyProt/Service/ConfigValidator.hpp"
#include "MiniTest.hpp"

#include <string>
#include <vector>

using MyProt::Core::Expected;
using MyProt::Service::ConfigValidator;
using MyProt::Service::ValidationResult;

namespace {

std::string ProtocolJson(const std::string& framing) {
    return std::string("{\"protocolName\":\"framing-test\",")
         + "\"transport\":{\"type\":\"Tcp\",\"defaultPort\":502},"
         + "\"framing\":" + framing + ","
         + "\"operations\":{\"Read\":{\"requestTemplate\":[\"00\"],"
         + "\"responseParser\":{}}},\"handshake\":[]}";
}

ValidationResult Validate(const std::string& framing) {
    const Expected<ValidationResult> out =
        ConfigValidator::validateProtocolJson(ProtocolJson(framing));
    if (!out.has_value()) {
        ValidationResult failed;
        failed.addError("parse failed: " + out.error().message);
        return failed;
    }
    return out.value();
}

bool HasErrorContaining(const ValidationResult& r, const std::string& needle) {
    for (size_t i = 0; i < r.errors.size(); ++i) {
        if (r.errors[i].find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(FramingSpecValidationTest, VarintRequiresZeroLengthFieldLength) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,"
        "\"lengthFieldLength\":2,\"lengthFieldEncoding\":\"Varint\"}");
    EXPECT_TRUE(HasErrorContaining(r, "lengthFieldLength 必须为 0"));
}

TEST(FramingSpecValidationTest, VarintMaxBytesMustBeWithinOneToFour) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,"
        "\"lengthFieldLength\":0,\"lengthFieldEncoding\":\"Varint\","
        "\"varintMaxBytes\":5}");
    EXPECT_TRUE(HasErrorContaining(r, "varintMaxBytes"));
}

TEST(FramingSpecValidationTest, DuplicateHeaderFieldNamesAreRejected) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,\"lengthFieldLength\":2,"
        "\"headerFields\":[{\"name\":\"type\",\"bitOffset\":0,\"bitWidth\":4},"
        "{\"name\":\"type\",\"bitOffset\":4,\"bitWidth\":4}]}");
    EXPECT_TRUE(HasErrorContaining(r, "name 重复"));
}

TEST(FramingSpecValidationTest, DispatchFieldMustExistInHeaderFields) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,\"lengthFieldLength\":2,"
        "\"headerFields\":[{\"name\":\"type\",\"bitOffset\":0,\"bitWidth\":4}],"
        "\"dispatch\":{\"field\":\"kind\",\"cases\":[{\"value\":3,\"type\":3}]}}");
    EXPECT_TRUE(HasErrorContaining(r, "dispatch.field"));
}

TEST(FramingSpecValidationTest, RepeatNeedsPositiveStrideAndKnownCountField) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,\"lengthFieldLength\":2,"
        "\"headerFields\":[{\"name\":\"count\",\"bitOffset\":8,\"bitWidth\":8}],"
        "\"repeat\":{\"countField\":\"count\",\"stride\":0,\"name\":\"records\"}}");
    EXPECT_TRUE(HasErrorContaining(r, "repeat.stride"));
}

TEST(FramingSpecValidationTest, BitFieldBeyondMaxFrameSizeIsRejected) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,\"lengthFieldLength\":2,"
        "\"maxFrameSize\":4,"
        "\"headerFields\":[{\"name\":\"wide\",\"bitOffset\":30,\"bitWidth\":8}]}");
    EXPECT_TRUE(HasErrorContaining(r, "位域越界"));
}

TEST(FramingSpecValidationTest, ValidSpecProducesNoFramingErrors) {
    const ValidationResult r = Validate(
        "{\"type\":\"LengthField\",\"lengthFieldOffset\":1,\"lengthFieldLength\":0,"
        "\"lengthFieldEncoding\":\"Varint\",\"varintMaxBytes\":4,"
        "\"headerFields\":[{\"name\":\"type\",\"bitOffset\":0,\"bitWidth\":4},"
        "{\"name\":\"count\",\"bitOffset\":8,\"bitWidth\":8}],"
        "\"dispatch\":{\"field\":\"type\",\"cases\":[{\"value\":3,\"type\":3,\"name\":\"Publish\"}]},"
        "\"repeat\":{\"countField\":\"count\",\"stride\":2,\"offset\":2}}");
    EXPECT_FALSE(HasErrorContaining(r, "lengthFieldEncoding"));
    EXPECT_FALSE(HasErrorContaining(r, "lengthFieldLength"));
    EXPECT_FALSE(HasErrorContaining(r, "varintMaxBytes"));
    EXPECT_FALSE(HasErrorContaining(r, "headerFields"));
    EXPECT_FALSE(HasErrorContaining(r, "dispatch"));
    EXPECT_FALSE(HasErrorContaining(r, "repeat"));
}

// ── UDP 数据报传输 (ADR-0002) ──

namespace {

std::string ProtocolWithTransport(const std::string& transport) {
    return std::string("{\"protocolName\":\"udp-test\",")
         + "\"transport\":" + transport + ","
         + "\"framing\":{\"type\":\"Fixed\",\"fixedLength\":8},"
         + "\"operations\":{\"Read\":{\"requestTemplate\":[\"00\"],"
         + "\"responseParser\":{}}},\"handshake\":[]}";
}

} // namespace

// Udp 传输判别值被解析器接受, 且 defaultPort 落到 udp 分支。
TEST(UdpTransportValidationTest, ParsesUdpTransportAndDefaultPort) {
    const Expected<MyProt::Core::ProtocolConfig> parsed =
        ConfigValidator::parseProtocolJson(ProtocolWithTransport(
            "{\"type\":\"Udp\",\"defaultPort\":5000}"));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed.value().transport.type == MyProt::Core::TransportType::Udp);
    EXPECT_EQ(parsed.value().transport.udp.defaultPort, 5000);
}

// Udp 协议在协议层校验不报"判别值越界", 也不报未知 transport.type。
TEST(UdpTransportValidationTest, UdpIsAcceptedAtProtocolLayer) {
    const Expected<ValidationResult> out = ConfigValidator::validateProtocolJson(
        ProtocolWithTransport("{\"type\":\"Udp\",\"defaultPort\":5000}"));
    ASSERT_TRUE(out.has_value());
    EXPECT_FALSE(HasErrorContaining(out.value(), "判别值越界"));
    EXPECT_FALSE(HasErrorContaining(out.value(), "未知 transport.type"));
}

// 未知传输判别值仍被拒绝 (确认新增 Udp 未放宽枚举门禁)。
TEST(UdpTransportValidationTest, UnknownTransportStillRejected) {
    const Expected<ValidationResult> out = ConfigValidator::validateProtocolJson(
        ProtocolWithTransport("{\"type\":\"Ftp\",\"defaultPort\":21}"));
    EXPECT_FALSE(out.has_value());
}

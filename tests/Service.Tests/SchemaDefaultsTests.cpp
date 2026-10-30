// tests/Service.Tests/SchemaDefaultsTests.cpp
// SchemaRegistry 默认值 ↔ Config POCO 默认构造值 一致性护栏。
//
// 背景: 字段契约当前分布在 Core/Config.hpp（默认值）、SchemaRegistry.cpp（UI 元数据）
//       与 ConfigDeepValidator.cpp（必填/范围）三处。本测试锁定前两处，防止
//       "UI 显示默认 3、运行时实际 5" 这类静默漂移；新增字段时若忘记同步会在此失败。
//
// 约定: 只比较"单值默认"。判别联合（transport/framing）的默认值按分支分别断言——
//       schema 是扁平表，同名 maxFrameSize 在 LengthField/Silence/Message 三支的
//       POCO 默认值并不同（1024 / 256 / 8），schema 的 default 只发布 LengthField 值。

#include "MyProt/Service/SchemaRegistry.hpp"
#include "MyProt/Core/Config.hpp"
#include "MiniTest.hpp"

#include <string>
#include <vector>

namespace {

using MyProt::Service::FieldDescriptor;
typedef std::vector<FieldDescriptor> Fields;

const FieldDescriptor* Find(const Fields& fs, const std::string& name) {
    for (std::size_t i = 0; i < fs.size(); ++i) {
        if (fs[i].name == name) return &fs[i];
    }
    return 0;
}

const FieldDescriptor* Child(const FieldDescriptor* parent, const std::string& name) {
    if (!parent) return 0;
    return Find(parent->children, name);
}

std::string IntStr(int v) { return std::to_string(v); }
std::string UIntStr(unsigned v) { return std::to_string(v); }
std::string BoolStr(bool v) { return v ? "true" : "false"; }

/// 顶层字段默认值断言
void ExpectDefault(const Fields& fs, const char* name, const std::string& expected) {
    const FieldDescriptor* fd = Find(fs, name);
    ASSERT_TRUE(fd != 0);
    EXPECT_EQ(fd->defaultValue, expected);
}

/// 二级字段默认值断言 (object -> child)
void ExpectDefault(const Fields& fs, const char* parent, const char* name,
                   const std::string& expected) {
    const FieldDescriptor* p = Find(fs, parent);
    ASSERT_TRUE(p != 0);
    const FieldDescriptor* c = Child(p, name);
    ASSERT_TRUE(c != 0);
    EXPECT_EQ(c->defaultValue, expected);
}

/// 三级字段默认值断言 (object -> object -> child)
void ExpectDefault(const Fields& fs, const char* grand, const char* parent,
                   const char* name, const std::string& expected) {
    const FieldDescriptor* g = Find(fs, grand);
    ASSERT_TRUE(g != 0);
    const FieldDescriptor* p = Child(g, parent);
    ASSERT_TRUE(p != 0);
    const FieldDescriptor* c = Child(p, name);
    ASSERT_TRUE(c != 0);
    EXPECT_EQ(c->defaultValue, expected);
}

std::string TransportTypeName(MyProt::Core::TransportType t) {
    switch (t) {
        case MyProt::Core::TransportType::Tcp:    return "Tcp";
        case MyProt::Core::TransportType::Tls:    return "Tls";
        case MyProt::Core::TransportType::Serial: return "Serial";
        case MyProt::Core::TransportType::Udp:    return "Udp";
    }
    return "";
}

std::string FramingTypeName(MyProt::Core::FramingType t) {
    switch (t) {
        case MyProt::Core::FramingType::LengthField: return "LengthField";
        case MyProt::Core::FramingType::Fixed:       return "Fixed";
        case MyProt::Core::FramingType::Silence:     return "Silence";
        case MyProt::Core::FramingType::Message:     return "Message";
    }
    return "";
}

std::string ByteOrderName(MyProt::Core::ByteOrder o) {
    switch (o) {
        case MyProt::Core::ByteOrder::BigEndian:         return "BigEndian";
        case MyProt::Core::ByteOrder::LittleEndian:      return "LittleEndian";
        case MyProt::Core::ByteOrder::WordBigByteLittle: return "WordBigByteLittle";
        case MyProt::Core::ByteOrder::WordLittleByteBig: return "WordLittleByteBig";
    }
    return "";
}

std::string SerialParityName(MyProt::Core::SerialTransportConfig::Parity p) {
    switch (p) {
        case MyProt::Core::SerialTransportConfig::Parity::None: return "None";
        case MyProt::Core::SerialTransportConfig::Parity::Odd:  return "Odd";
        case MyProt::Core::SerialTransportConfig::Parity::Even: return "Even";
    }
    return "";
}

std::string SerialStopBitsName(MyProt::Core::SerialTransportConfig::StopBits s) {
    switch (s) {
        case MyProt::Core::SerialTransportConfig::StopBits::One: return "One";
        case MyProt::Core::SerialTransportConfig::StopBits::Two: return "Two";
    }
    return "";
}

} // namespace

// ── 根层 (tags.json): schemaVersion / resilience / webApi ──
TEST(SchemaDefaultsTest, RootSectionMatchesConfigPoco) {
    const Fields& root = MyProt::Service::SchemaRegistry::RootFields();
    const MyProt::Core::ConfigRoot cfg;
    const MyProt::Core::ResilienceConfig res;
    const MyProt::Core::WebApiConfig web;

    ExpectDefault(root, "schemaVersion",
                  std::to_string(MyProt::Core::kSupportedSchemaVersion));

    ExpectDefault(root, "resilience", "maxAttempts", IntStr(res.maxAttempts));
    ExpectDefault(root, "resilience", "backoffBaseMs", IntStr(res.backoffBaseMs));
    ExpectDefault(root, "resilience", "backoffMaxMs", IntStr(res.backoffMaxMs));
    ExpectDefault(root, "resilience", "failureThreshold", IntStr(res.failureThreshold));
    ExpectDefault(root, "resilience", "cooldownMs", IntStr(res.cooldownMs));
    ExpectDefault(root, "resilience", "halfOpenProbes", IntStr(res.halfOpenProbes));

    ExpectDefault(root, "webApi", "bindAddress", web.bindAddress);
    ExpectDefault(root, "webApi", "certFile", web.certFile);
    ExpectDefault(root, "webApi", "keyFile", web.keyFile);
    ExpectDefault(root, "webApi", "requireAuth", BoolStr(web.requireAuth));
    ExpectDefault(root, "webApi", "rateLimitRps", IntStr(web.rateLimitRps));
    ExpectDefault(root, "webApi", "rateLimitBurst", IntStr(web.rateLimitBurst));
    ExpectDefault(root, "webApi", "webRoot", web.webRoot);

    // ConfigRoot 构造函数的 schemaVersion 也必须跟随单一真源
    EXPECT_EQ(cfg.schemaVersion, MyProt::Core::kSupportedSchemaVersion);
}

// ── 协议层: transport / framing / operation / handshake ──
TEST(SchemaDefaultsTest, ProtocolSectionMatchesConfigPoco) {
    const Fields& p = MyProt::Service::SchemaRegistry::ProtocolFields();
    const MyProt::Core::ProtocolConfig proto;
    const MyProt::Core::TransportConfig tr;
    const MyProt::Core::TcpTransportConfig tcp;
    const MyProt::Core::TlsTransportConfig tls;
    const MyProt::Core::SerialTransportConfig ser;
    const MyProt::Core::FramingConfig framing;
    const MyProt::Core::LengthFieldConfig lf;
    const MyProt::Core::SilenceConfig silence;
    const MyProt::Core::MessageConfig message;
    const MyProt::Core::ResponseParserConfig parser;
    const MyProt::Core::OperationConfig operation;
    const MyProt::Core::HandshakeStep step;

    ExpectDefault(p, "schemaVersion", std::to_string(proto.schemaVersion));
    ExpectDefault(p, "maxSpanBytes", IntStr(proto.maxSpanBytes));
    EXPECT_EQ(proto.maxSpanBytes, MyProt::Core::kDefaultMaxSpanBytes);

    ExpectDefault(p, "transport", "type", TransportTypeName(tr.type));
    ExpectDefault(p, "transport", "defaultPort", UIntStr(tcp.defaultPort));
    ExpectDefault(p, "transport", "verifyServer", BoolStr(tls.verifyServer));
    ExpectDefault(p, "transport", "baudRate", UIntStr(ser.baudRate));
    ExpectDefault(p, "transport", "dataBits", UIntStr(ser.dataBits));
    ExpectDefault(p, "transport", "parity", SerialParityName(ser.parity));
    ExpectDefault(p, "transport", "stopBits", SerialStopBitsName(ser.stopBits));

    ExpectDefault(p, "framing", "type", FramingTypeName(framing.type));
    ExpectDefault(p, "framing", "lengthFieldOffset", IntStr(lf.lengthFieldOffset));
    ExpectDefault(p, "framing", "lengthIncludesHeader", BoolStr(lf.lengthIncludesHeader));
    ExpectDefault(p, "framing", "byteOrder", ByteOrderName(lf.byteOrder));
    ExpectDefault(p, "framing", "headerLength", IntStr(lf.headerLength));
    ExpectDefault(p, "framing", "lengthAdjustment", IntStr(lf.lengthAdjustment));
    // 扁平表的 default 只发布 LengthField 分支 (Silence/Message 各为 256/8)
    ExpectDefault(p, "framing", "maxFrameSize", IntStr(lf.maxFrameSize));
    ExpectDefault(p, "framing", "charTimeUs", IntStr(silence.charTimeUs));
    ExpectDefault(p, "framing", "frameGapUs", IntStr(silence.frameGapUs));
    ExpectDefault(p, "framing", "idFieldLength", IntStr(message.idFieldLength));

    ExpectDefault(p, "operations", "responseParser", "dataStartIndex",
                  IntStr(parser.dataStartIndex));
    ExpectDefault(p, "operations", "expectResponse", BoolStr(operation.expectResponse));
    ExpectDefault(p, "handshake", "timeoutMs", IntStr(step.timeoutMs));
}

// ── 设备层: connection / requestTimeoutMs / resilience 覆盖 ──
TEST(SchemaDefaultsTest, DeviceSectionMatchesConfigPoco) {
    const Fields& d = MyProt::Service::SchemaRegistry::DeviceFields();
    const MyProt::Core::ConnectionConfig conn;
    const MyProt::Core::DeviceConfig dev;
    const MyProt::Core::ResilienceConfig res;

    ExpectDefault(d, "connection", "port", UIntStr(conn.port));
    ExpectDefault(d, "connection", "timeoutMs", IntStr(conn.timeoutMs));
    ExpectDefault(d, "requestTimeoutMs", IntStr(dev.requestTimeoutMs));
    ExpectDefault(d, "resilience", "maxAttempts", IntStr(res.maxAttempts));
    ExpectDefault(d, "resilience", "cooldownMs", IntStr(res.cooldownMs));
}

// ── 标签层: 采集/写能力默认值 ──
TEST(SchemaDefaultsTest, TagSectionMatchesConfigPoco) {
    const Fields& t = MyProt::Service::SchemaRegistry::TagFields();
    const MyProt::Core::TagDefinition tag;

    ExpectDefault(t, "scanRateMs", IntStr(tag.scanRateMs));
    ExpectDefault(t, "finalType", tag.finalType);
    ExpectDefault(t, "coalesce", BoolStr(tag.coalesce));
    ExpectDefault(t, "bitOffset", IntStr(tag.bitOffset));
    ExpectDefault(t, "direction", tag.direction);
    ExpectDefault(t, "writeVariable", tag.writeVariable);
    EXPECT_EQ(tag.writeVariable, std::string(MyProt::Core::kDefaultWriteValueVariable));
}

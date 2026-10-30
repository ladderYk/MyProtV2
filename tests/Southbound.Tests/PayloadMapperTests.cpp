// tests/Southbound.Tests/PayloadMapperTests.cpp — 入站载荷映射单元测试
// 覆盖: topic 段映射、JSON 单值/批量/标量映射、时间戳归一化、finalType 转换。

#include "MyProt/Southbound/PayloadMapper.hpp"
#include "MiniTest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using MyProt::Southbound::MappingItem;
using MyProt::Southbound::PayloadMapper;

} // namespace

TEST(PayloadMapperTest, TopicSegmentIsOneBased) {
    std::string tag;
    EXPECT_TRUE(PayloadMapper::MapTopicTag("plant/PLC-1/Temp", 3, tag));
    EXPECT_EQ(tag, std::string("Temp"));

    EXPECT_TRUE(PayloadMapper::MapTopicTag("plant/PLC-1/Temp", 1, tag));
    EXPECT_EQ(tag, std::string("plant"));

    EXPECT_FALSE(PayloadMapper::MapTopicTag("plant/PLC-1/Temp", 4, tag));
    EXPECT_FALSE(PayloadMapper::MapTopicTag("plant/PLC-1/Temp", 0, tag));
}

TEST(PayloadMapperTest, SingleValueObject) {
    std::vector<MappingItem> items;
    std::string err;
    EXPECT_TRUE(PayloadMapper::MapJson(
        "{\"tag\":\"T1\",\"value\":12.5,\"ts\":1700000000}",
        "tag", "value", "ts", "", items, err));
    ASSERT_EQ(items.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(items[0].tagName, std::string("T1"));
    EXPECT_EQ(items[0].timestampMs, static_cast<int64_t>(1700000000000LL));

    MyProt::Core::TypedValue value;
    EXPECT_TRUE(PayloadMapper::ToTypedValue(items[0].value, "Double", value, err));
    EXPECT_EQ(static_cast<int>(value.type), static_cast<int>(MyProt::Core::ValueType::Double));
    EXPECT_EQ(value.d, 12.5);
}

TEST(PayloadMapperTest, SingleValueFallsBackToDefaultTag) {
    std::vector<MappingItem> items;
    std::string err;
    EXPECT_TRUE(PayloadMapper::MapJson("{\"value\":7}", "tag", "value", "", "PLC.Temp",
                                       items, err));
    ASSERT_EQ(items.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(items[0].tagName, std::string("PLC.Temp"));
    EXPECT_EQ(items[0].timestampMs, static_cast<int64_t>(0));
}

TEST(PayloadMapperTest, BatchObjectMapsEveryKey) {
    std::vector<MappingItem> items;
    std::string err;
    EXPECT_TRUE(PayloadMapper::MapJson("{\"A\":1,\"B\":true}", "tag", "value", "",
                                       "", items, err));
    ASSERT_EQ(items.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(items[0].tagName, std::string("A"));
    EXPECT_EQ(items[1].tagName, std::string("B"));
}

TEST(PayloadMapperTest, ScalarRequiresDefaultTag) {
    std::vector<MappingItem> items;
    std::string err;
    EXPECT_FALSE(PayloadMapper::MapJson("42", "tag", "value", "", "", items, err));
    EXPECT_FALSE(err.empty());

    EXPECT_TRUE(PayloadMapper::MapJson("42", "", "", "", "T", items, err));
    ASSERT_EQ(items.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(items[0].tagName, std::string("T"));
}

TEST(PayloadMapperTest, MalformedJsonRejected) {
    std::vector<MappingItem> items;
    std::string err;
    EXPECT_FALSE(PayloadMapper::MapJson("{not json", "", "", "", "T", items, err));
    EXPECT_FALSE(err.empty());
}

TEST(PayloadMapperTest, AutoTypeInference) {
    std::string err;
    MyProt::Core::TypedValue value;

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json(true), "Auto", value, err));
    EXPECT_TRUE(value.type == MyProt::Core::ValueType::Bool && value.b);

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json(42), "Auto", value, err));
    EXPECT_EQ(static_cast<int>(value.type), static_cast<int>(MyProt::Core::ValueType::Int64));
    EXPECT_EQ(value.i, static_cast<int64_t>(42));

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json("abc"), "Auto", value, err));
    EXPECT_EQ(static_cast<int>(value.type), static_cast<int>(MyProt::Core::ValueType::String));
    EXPECT_EQ(value.str, std::string("abc"));
}

TEST(PayloadMapperTest, DeclaredTypesWin) {
    std::string err;
    MyProt::Core::TypedValue value;

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json("12"), "UInt16", value, err));
    EXPECT_EQ(static_cast<int>(value.type), static_cast<int>(MyProt::Core::ValueType::UInt16));
    EXPECT_EQ(value.u, static_cast<uint64_t>(12));

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json(0), "Bool", value, err));
    EXPECT_TRUE(value.type == MyProt::Core::ValueType::Bool && !value.b);

    // 数组 -> ByteArray; 十六进制字符串同样接受
    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json::array({1, 255}), "ByteArray",
                                            value, err));
    EXPECT_EQ(value.bytes.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<int>(value.bytes[1]), 255);

    EXPECT_TRUE(PayloadMapper::ToTypedValue(nlohmann::json("0A0b"), "ByteArray", value, err));
    EXPECT_EQ(value.bytes.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(static_cast<int>(value.bytes[0]), 10);
    EXPECT_EQ(static_cast<int>(value.bytes[1]), 11);
}

TEST(PayloadMapperTest, OutOfRangeRejected) {
    std::string err;
    MyProt::Core::TypedValue value;
    EXPECT_FALSE(PayloadMapper::ToTypedValue(nlohmann::json(-1), "UInt16", value, err));
    EXPECT_FALSE(err.empty());
    EXPECT_FALSE(PayloadMapper::ToTypedValue(nlohmann::json("x"), "Int32", value, err));
}

// tests/Southbound.Tests/TagValueSinkTests.cpp — 单一写入口语义单元测试
// 覆盖: Good/Bad/Uncertain 三条路径、valueChanged 自比较、converters、静默保留上次值。

#include "MyProt/Southbound/TagValueSink.hpp"
#include "MiniTest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using MyProt::Core::QualityCode;
using MyProt::Core::TagDefinition;
using MyProt::Core::TagValue;
using MyProt::Core::TypedValue;
using MyProt::Core::ValueType;
using MyProt::Southbound::TagValueSink;

struct Capture {
    std::vector<TagValue> values;

    TagValueSink::StoreWriter Writer() {
        Capture* self = this;
        return [self](const std::vector<TagValue>& batch) {
            self->values.insert(self->values.end(), batch.begin(), batch.end());
        };
    }
};

TypedValue Number(double d) {
    TypedValue v;
    v.type = ValueType::Double;
    v.d = d;
    return v;
}

} // namespace

TEST(TagValueSinkTest, EmitWritesGoodWithChangeFlag) {
    Capture capture;
    TagValueSink sink(capture.Writer());

    TagDefinition tag;
    tag.name = "D1.Temp";
    tag.deviceId = "D1";
    tag.finalType = "Double";

    std::vector<uint8_t> raw;
    raw.push_back(1);
    sink.Emit(tag, Number(21.5), 1000, raw);
    ASSERT_EQ(capture.values.size(), static_cast<std::size_t>(1));
    EXPECT_EQ(capture.values[0].tagName, std::string("D1.Temp"));
    EXPECT_EQ(capture.values[0].deviceId, std::string("D1"));
    EXPECT_TRUE(capture.values[0].quality == QualityCode::Good);
    EXPECT_TRUE(capture.values[0].valueChanged);      // 首次一定算变化
    EXPECT_EQ(capture.values[0].timestamp, static_cast<int64_t>(1000));
    EXPECT_EQ(capture.values[0].rawData.size(), static_cast<std::size_t>(1));

    sink.Emit(tag, Number(21.5), 2000, raw);
    EXPECT_FALSE(capture.values[1].valueChanged);     // 同值不再算变化

    sink.Emit(tag, Number(22.0), 3000, raw);
    EXPECT_TRUE(capture.values[2].valueChanged);
}

TEST(TagValueSinkTest, EmitAppliesConverters) {
    Capture capture;
    TagValueSink sink(capture.Writer());

    TagDefinition tag;
    tag.name = "D1.Pressure";
    tag.deviceId = "D1";
    tag.finalType = "Double";
    MyProt::Core::ScaleConverter first;
    first.k = 2.0;
    first.b = 1.0;
    tag.converters.push_back(first);

    std::vector<uint8_t> raw;
    sink.Emit(tag, Number(3.0), 0, raw);

    ASSERT_EQ(capture.values.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(capture.values[0].typedValue.type == ValueType::Double);
    EXPECT_EQ(capture.values[0].typedValue.d, 7.0);   // 3 * 2 + 1
}

TEST(TagValueSinkTest, EmitBadCarriesParseError) {
    Capture capture;
    TagValueSink sink(capture.Writer());

    TagDefinition tag;
    tag.name = "D1.Temp";
    tag.deviceId = "D1";

    std::vector<uint8_t> raw;
    raw.push_back('{');
    sink.EmitBad(tag, "JSON 解析失败", 500, raw);

    ASSERT_EQ(capture.values.size(), static_cast<std::size_t>(1));
    EXPECT_TRUE(capture.values[0].quality == QualityCode::Bad);
    EXPECT_TRUE(capture.values[0].typedValue.type == ValueType::Empty);
    EXPECT_FALSE(capture.values[0].lastError.message.empty());
    EXPECT_EQ(capture.values[0].lastError.code,
              MyProt::Core::Error::Code::ParseError);
}

TEST(TagValueSinkTest, SilentKeepsLastValueAndDegradesQuality) {
    Capture capture;
    TagValueSink sink(capture.Writer());

    TagDefinition tag;
    tag.name = "D1.Temp";
    tag.deviceId = "D1";

    std::vector<uint8_t> raw;
    sink.Emit(tag, Number(18.0), 100, raw);
    sink.EmitSilent(tag, 900);

    ASSERT_EQ(capture.values.size(), static_cast<std::size_t>(2));
    EXPECT_TRUE(capture.values[1].quality == QualityCode::Uncertain);
    EXPECT_FALSE(capture.values[1].valueChanged);
    EXPECT_TRUE(capture.values[1].typedValue.type == ValueType::Double);
    EXPECT_EQ(capture.values[1].typedValue.d, 18.0);
}

TEST(TagValueSinkTest, ResetForgetsChangeBaseline) {
    Capture capture;
    TagValueSink sink(capture.Writer());

    TagDefinition tag;
    tag.name = "D1.Temp";
    tag.deviceId = "D1";

    std::vector<uint8_t> raw;
    sink.Emit(tag, Number(5.0), 100, raw);
    sink.Emit(tag, Number(5.0), 200, raw);
    EXPECT_FALSE(capture.values[1].valueChanged);

    sink.Reset();
    sink.Emit(tag, Number(5.0), 300, raw);
    EXPECT_TRUE(capture.values[2].valueChanged);
}

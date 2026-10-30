// tests/Southbound.Tests/LatestValueStoreTests.cpp — 变化检测语义单元测试
// 契约: 首次写入算变化; type+值 或质量码变化算变化; 时间戳/requestId 不参与比较。
// 这条口径同时供轮询、南向推送、SSE / HTTP / MQTT 的 "changed" 字段使用。

#include "MyProt/Polling/LatestValueStore.hpp"
#include "MiniTest.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using MyProt::Core::QualityCode;
using MyProt::Core::TagValue;
using MyProt::Core::ValueType;
using MyProt::Polling::LatestValueStore;

TagValue MakeDouble(const std::string& name, double v,
                    QualityCode quality = QualityCode::Good,
                    int64_t ts = 1000) {
    TagValue tv;
    tv.tagName = name;
    tv.deviceId = "D1";
    tv.typedValue.type = ValueType::Double;
    tv.typedValue.d = v;
    tv.timestamp = ts;
    tv.quality = quality;
    return tv;
}

void Put(LatestValueStore& store, const TagValue& tv) {
    std::vector<TagValue> one(1, tv);
    store.Update(one);
}

bool Changed(const LatestValueStore& store, const std::string& name) {
    TagValue out;
    return store.Lookup(name, out) && out.valueChanged;
}

} // namespace

TEST(LatestValueStoreTest, FirstSampleCountsAsChanged) {
    LatestValueStore store;
    Put(store, MakeDouble("T", 1.5));

    EXPECT_EQ(store.Count(), static_cast<std::size_t>(1));
    EXPECT_TRUE(Changed(store, "T"));
}

TEST(LatestValueStoreTest, TimestampOnlyDoesNotCountAsChange) {
    LatestValueStore store;
    Put(store, MakeDouble("T", 1.5, QualityCode::Good, 1000));
    Put(store, MakeDouble("T", 1.5, QualityCode::Good, 2000));

    EXPECT_FALSE(Changed(store, "T"));
}

TEST(LatestValueStoreTest, ValueOrQualityChangeSetsFlag) {
    LatestValueStore store;
    Put(store, MakeDouble("T", 1.5));

    Put(store, MakeDouble("T", 2.5));                  // 值变化
    EXPECT_TRUE(Changed(store, "T"));

    Put(store, MakeDouble("T", 2.5, QualityCode::Bad)); // 值不变、质量变化
    EXPECT_TRUE(Changed(store, "T"));

    Put(store, MakeDouble("T", 2.5, QualityCode::Bad)); // 两者都不变
    EXPECT_FALSE(Changed(store, "T"));
}

TEST(LatestValueStoreTest, TypeChangeSetsFlag) {
    LatestValueStore store;
    Put(store, MakeDouble("T", 1.0));

    TagValue asU16 = MakeDouble("T", 0.0);
    asU16.typedValue.type = ValueType::UInt16;
    asU16.typedValue.u = 1;
    Put(store, asU16);

    EXPECT_TRUE(Changed(store, "T"));
}

TEST(LatestValueStoreTest, ByteArrayComparesByContent) {
    LatestValueStore store;

    TagValue a;
    a.tagName = "B";
    a.typedValue.type = ValueType::ByteArray;
    a.typedValue.bytes.push_back(1);
    a.typedValue.bytes.push_back(2);
    Put(store, a);

    TagValue same = a;
    Put(store, same);
    EXPECT_FALSE(Changed(store, "B"));

    TagValue diff = a;
    diff.typedValue.bytes[1] = 3;
    Put(store, diff);
    EXPECT_TRUE(Changed(store, "B"));
}

TEST(LatestValueStoreTest, NanAlwaysCountsAsChange) {
    LatestValueStore store;
    const double nan = std::numeric_limits<double>::quiet_NaN();

    Put(store, MakeDouble("T", nan));
    EXPECT_TRUE(Changed(store, "T"));

    Put(store, MakeDouble("T", nan));   // NaN != NaN → 每次采样都算变化
    EXPECT_TRUE(Changed(store, "T"));
}

TEST(LatestValueStoreTest, ClearDropsBaseline) {
    LatestValueStore store;
    Put(store, MakeDouble("T", 1.5));
    store.Clear();

    EXPECT_EQ(store.Count(), static_cast<std::size_t>(0));
    Put(store, MakeDouble("T", 1.5));
    EXPECT_TRUE(Changed(store, "T"));
}

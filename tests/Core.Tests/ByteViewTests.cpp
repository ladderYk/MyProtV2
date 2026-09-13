// tests/Core.Tests/ByteViewTests.cpp — ByteView 单元测试占位

#include "MyProt/Core/ByteView.hpp"
#include <gtest/gtest.h>

using namespace MyProt::Core;

TEST(ByteViewTest, DefaultConstruction) {
    ByteView bv;
    EXPECT_TRUE(bv.empty());
    EXPECT_EQ(bv.size, 0u);
}

TEST(ByteViewTest, FromBytes) {
    Bytes bytes = {0x01, 0x02, 0x03};
    ByteView bv(bytes);
    EXPECT_FALSE(bv.empty());
    EXPECT_EQ(bv.size, 3u);
    EXPECT_EQ(bv[0], 0x01);
    EXPECT_EQ(bv[1], 0x02);
    EXPECT_EQ(bv[2], 0x03);
}

TEST(ByteViewTest, FromPointer) {
    uint8_t data[] = {0xAA, 0xBB};
    ByteView bv(data, 2);
    EXPECT_EQ(bv.size, 2u);
    EXPECT_EQ(bv[0], 0xAA);
    EXPECT_EQ(bv[1], 0xBB);
}

// tests/Core.Tests/ExpectedTests.cpp — Expected<T> 单元测试占位
// 测试框架: GoogleTest 1.8.x (VS2015 兼容, ADR-0010 §4)

#include "MyProt/Core/Expected.hpp"
#include <gtest/gtest.h>

using namespace MyProt::Core;

TEST(ExpectedTest, ValueConstruction) {
    Expected<int> e(42);
    EXPECT_TRUE(e.has_value());
    EXPECT_EQ(e.value(), 42);
}

TEST(ExpectedTest, ErrorConstruction) {
    Expected<int> e = Unexpected(Error::Code::Timeout, "test timeout");
    EXPECT_FALSE(e.has_value());
    EXPECT_EQ(e.error().code, Error::Code::Timeout);
    EXPECT_EQ(e.error().message, "test timeout");
}

TEST(ExpectedTest, ValueOr) {
    Expected<int> e = Unexpected(Error::Code::Timeout);
    EXPECT_EQ(e.value_or(-1), -1);

    Expected<int> e2(42);
    EXPECT_EQ(e2.value_or(-1), 42);
}

TEST(ExpectedTest, VoidSpecialization) {
    Expected<void> e;
    EXPECT_TRUE(e.has_value());

    Expected<void> err = Unexpected(Error::Code::ConfigError, "bad config");
    EXPECT_FALSE(err.has_value());
    EXPECT_EQ(err.error().code, Error::Code::ConfigError);
}

TEST(ExpectedTest, MonadicMap) {
    Expected<int> e(10);
    auto result = e.map([](int& v) { return v * 2; });
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 20);
}

TEST(ExpectedTest, MonadicMapError) {
    Expected<int> e = Unexpected(Error::Code::ParseError);
    auto result = e.map([](int& v) { return v * 2; });
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, Error::Code::ParseError);
}

TEST(ExpectedTest, IsRetryable) {
    EXPECT_TRUE(IsRetryable(Error::Code::Timeout));
    EXPECT_TRUE(IsRetryable(Error::Code::ConnectionRefused));
    EXPECT_TRUE(IsRetryable(Error::Code::ConnectionClosed));
    EXPECT_TRUE(IsRetryable(Error::Code::Busy));
    EXPECT_FALSE(IsRetryable(Error::Code::ParseError));
    EXPECT_FALSE(IsRetryable(Error::Code::ConfigError));
    EXPECT_FALSE(IsRetryable(Error::Code::WriteTimeout));
}

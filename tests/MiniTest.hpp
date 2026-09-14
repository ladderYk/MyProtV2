// tests/MiniTest.hpp — 极简单元测试框架 (gtest 兼容子集)
//
// 为什么不用 GoogleTest: third_party/gtest 只有头文件, lib/*/gtest.lib 是 8 字节空归档
// (`!<arch>`)——setup.bat 也写着"请手动编译 gtest 并放置"。也就是说这个依赖从未真正就绪,
// 测试项目因此一直无法链接(见 ADR-0013 基础设施准入: 宁可用 100 行的自研件, 也不留坏依赖)。
//
// 覆盖面 = 现有测试实际用到的 API 子集:
//   TEST(Suite, Name) / EXPECT_TRUE / EXPECT_FALSE / EXPECT_EQ / ASSERT_TRUE
// 需要新断言时在此扩展, 保持"测试不依赖外部框架"。

#ifndef MYPROT_TESTS_MINITEST_HPP
#define MYPROT_TESTS_MINITEST_HPP

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>
#include <vector>

namespace MyProtTest {

struct Case {
    const char* suite;
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> reg;   // C++11: 函数内静态初始化线程安全
    return reg;
}

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        Registry().push_back(Case{suite, name, fn});
    }
};

inline int& FailCount() { static int n = 0; return n; }
inline int& AssertCount() { static int n = 0; return n; }

namespace Detail {

/// 值可否用 ostream 打印 (编译期探测; 不可打印则退化为 <value>)
template <typename T>
struct IsStreamable {
    template <typename U>
    static auto Test(int) -> decltype(std::declval<std::ostream&>() << std::declval<const U&>(),
                                      std::true_type());
    template <typename U>
    static std::false_type Test(...);
    static const bool value = decltype(Test<T>(0))::value;
};

template <typename T>
typename std::enable_if<IsStreamable<T>::value, void>::type Print(const T& v) {
    std::cout << v;
}

template <typename T>
typename std::enable_if<!IsStreamable<T>::value, void>::type Print(const T&) {
    std::cout << "<value>";
}

inline bool Report(const char* file, int line, const char* expr, bool ok, bool fatal) {
    ++AssertCount();
    if (ok) return true;
    ++FailCount();
    std::printf("    [FAIL] %s:%d: %s%s\n", file, line, expr, fatal ? " (致命, 中止该用例)" : "");
    std::fflush(stdout);
    return false;
}

} // namespace Detail

/// 运行全部用例; 返回失败数 (供 main 作为退出码)
inline int RunAll() {
    int failedCases = 0;
    std::printf("Running %d test case(s)\n", static_cast<int>(Registry().size()));
    for (size_t i = 0; i < Registry().size(); ++i) {
        const Case& c = Registry()[i];
        const int before = FailCount();
        std::printf("[ RUN      ] %s.%s\n", c.suite, c.name);
        c.fn();
        if (FailCount() == before) {
            std::printf("[       OK ] %s.%s\n", c.suite, c.name);
        } else {
            ++failedCases;
            std::printf("[  FAILED  ] %s.%s\n", c.suite, c.name);
        }
        std::fflush(stdout);
    }
    std::printf("--------------------------------------------------\n");
    std::printf("断言 %d 条, 失败 %d 条, 用例失败 %d / %d\n",
                AssertCount(), FailCount(), failedCases,
                static_cast<int>(Registry().size()));
    return FailCount() == 0 ? 0 : 1;
}

} // namespace MyProtTest

#define TEST(Suite, Name)                                                   \
    static void Suite##_##Name##_Body();                                    \
    static MyProtTest::Registrar Suite##_##Name##_Reg(                      \
        #Suite, #Name, &Suite##_##Name##_Body);                             \
    static void Suite##_##Name##_Body()

#define MT_EXPECT_(cond, text, fatal)                                       \
    do {                                                                    \
        if (!MyProtTest::Detail::Report(__FILE__, __LINE__, text, (cond), (fatal)) && (fatal)) { \
            return;                                                         \
        }                                                                   \
    } while (0)

#define SUCCEED()        ((void)0)   // 对应 gtest 同名宏: 显式标记成功
#define EXPECT_TRUE(x)   MT_EXPECT_((x), "EXPECT_TRUE(" #x ")", false)
#define EXPECT_FALSE(x)  MT_EXPECT_(!(x), "EXPECT_FALSE(" #x ")", false)
#define ASSERT_TRUE(x)   MT_EXPECT_((x), "ASSERT_TRUE(" #x ")", true)

#define MT_EXPECT_EQ_(a, b, fatal)                                          \
    do {                                                                    \
        const auto mt_a_ = (a);                                             \
        const auto mt_b_ = (b);                                             \
        const bool mt_ok_ = (mt_a_ == mt_b_);                               \
        if (!mt_ok_) {                                                      \
            std::cout << "      lhs = ";                                    \
            MyProtTest::Detail::Print(mt_a_);                               \
            std::cout << "\n      rhs = ";                                  \
            MyProtTest::Detail::Print(mt_b_);                               \
            std::cout << "\n";                                              \
        }                                                                   \
        if (!MyProtTest::Detail::Report(__FILE__, __LINE__, "EXPECT_EQ(" #a ", " #b ")", mt_ok_, (fatal)) && (fatal)) { \
            return;                                                         \
        }                                                                   \
    } while (0)

#define EXPECT_EQ(a, b)  MT_EXPECT_EQ_(a, b, false)
#define ASSERT_EQ(a, b)  MT_EXPECT_EQ_(a, b, true)

#endif // MYPROT_TESTS_MINITEST_HPP

// tests/MiniTestMain.cpp — 单元测试入口 (取代 gtest_main)
//
// 各测试项目共用本文件提供 main(); 退出码 = 失败断言数 (0 = 全绿, 可被 CI/脚本判定)。

#include "MiniTest.hpp"

int main() {
    return MyProtTest::RunAll();
}

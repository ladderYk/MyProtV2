// src/App/CrashDiagnostics.hpp — 崩溃取证 (terminate / SIGABRT / SEH + dbghelp 栈回溯)
// 方案1-S1: 从 RuntimeGlue.cpp 原样搬移 (零行为变更)。

#ifndef MYPROT_APP_CRASHDIAGNOSTICS_HPP
#define MYPROT_APP_CRASHDIAGNOSTICS_HPP

namespace MyProt { namespace App {

/// 安装崩溃取证: set_terminate / SIGABRT / SEH 未处理异常 + 预热 dbghelp 符号
void InstallCrashDiagnostics();

}} // namespace MyProt::App

#endif // MYPROT_APP_CRASHDIAGNOSTICS_HPP

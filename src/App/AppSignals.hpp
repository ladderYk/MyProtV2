// src/App/AppSignals.hpp — 进程停止信号与运行标志
// 方案1-S1: 从 RuntimeGlue.cpp 原样搬移 (零行为变更); RuntimeGlue.hpp 仍转发声明, main.cpp 无需改动。

#ifndef MYPROT_APP_APPSIGNALS_HPP
#define MYPROT_APP_APPSIGNALS_HPP

namespace MyProt { namespace App {

/// 进程是否应继续运行 (Ctrl+C / 关窗 → false)
bool IsRunning();

/// 安装 SIGINT/SIGTERM 处理 (Ctrl+C / 关窗)
void InstallStopSignals();

}} // namespace MyProt::App

#endif // MYPROT_APP_APPSIGNALS_HPP

// src/App/AppSignals.cpp — 进程停止信号与运行标志 (方案1-S1 从 RuntimeGlue.cpp 搬移, 零行为变更)

#include "AppSignals.hpp"

#include <atomic>
#include <csignal>

namespace MyProt { namespace App {
namespace {
std::atomic<bool> g_running(true);
void OnSignal(int) { g_running.store(false); }
} // namespace

bool IsRunning() { return g_running.load(); }

void InstallStopSignals() {
    ::signal(SIGINT, OnSignal);
    ::signal(SIGTERM, OnSignal);
}

}} // namespace MyProt::App

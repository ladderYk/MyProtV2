// src/App/RuntimeGlue.hpp — 生产/E2E 两进程共享胶水层 API
// 崩溃取证 / 停止信号 / 扩展路由 handler / ApplyRuntimeSync 运行时装配。
// 实现 (RuntimeGlue.cpp) 同时编入 MyProt.App 与 MyProt.E2E 两个工程。
// 运行时状态聚合见 AppContext.hpp; 路由注册见 HttpRouter.hpp。
#ifndef MYPROT_APP_RUNTIMEGLUE_HPP
#define MYPROT_APP_RUNTIMEGLUE_HPP

#include <iosfwd>
#include <string>
#include <utility>

#include "AppContext.hpp"

namespace MyProt { namespace App {

// ── 崩溃取证 / 运行控制 (生产入口专用) ──
void InstallCrashDiagnostics();     // SEH + dbghelp 栈回溯安装
bool IsRunning();
void InstallStopSignals();          // Ctrl+C / 关窗 → g_running=false

// ── URL 查询参数提取 ("/api/x?start=3&count=8", key=start → "3") ──
std::string QueryParam(const std::string& path, const std::string& key);

// ── WebApi 扩展路由 handler (两进程共用; method 校验在各 handler 内部) ──

/// /api/sim/* 仿真数据面: status 清单 / registers 读(GET) 写(POST)
std::pair<int, std::string> HandleSimApi(const HttpRequest& req,
                                         const SimServerMap& sims);

/// latest 快照 → JSON ({count, tags:[...]}) — /latest 与 SSE /stream 共用
std::string BuildLatestJson(const Polling::LatestValueStore& latestStore,
                            const std::string& deviceFilter);

/// GET /api/data/latest 实时数据快照 (?device= 过滤)
std::pair<int, std::string> HandleDataApi(
    const HttpRequest& req, const Polling::LatestValueStore& latestStore);

/// POST /api/data/write 单寄存器写 (io.post 全链, 与轮询回调串行)
std::pair<int, std::string> HandleWriteApi(const AppContext& ctx,
                                           const HttpRequest& req);

// ── reload 运行时联动 — 首次装配与热重载唯一代码路径 ──
// 重新加载配置目录 → 全停重建仿真器 → 清空实时快照 → post 到 io 线程
// 重装配引擎。新配置非法时保持旧运行态并返回错误 (配合 ConfigStore 自动回滚)。
Core::VoidExpected ApplyRuntimeSync(const std::string& configDir,
                                    AppContext& ctx, std::ostream* log);

}} // namespace MyProt::App

#endif // MYPROT_APP_RUNTIMEGLUE_HPP

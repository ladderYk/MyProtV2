// src/App/RuntimeGlue.hpp - glue-layer API shared by the production/E2E two processes
// Crash forensics / stop signal / extension-route handler / ApplyRuntimeSync runtime assembly.
// The implementation (RuntimeGlue.cpp) is compiled into both MyProt.App and MyProt.E2E projects.
// Runtime-state aggregation see AppContext.hpp; route registration see HttpRouter.hpp.
#ifndef MYPROT_APP_RUNTIMEGLUE_HPP
#define MYPROT_APP_RUNTIMEGLUE_HPP

#include <iosfwd>
#include <string>
#include <utility>

#include "AppContext.hpp"

namespace MyProt { namespace App {

// ── Crash forensics / run control (production entry only) ──
void InstallCrashDiagnostics();     // SEH + dbghelp stack-trace installation
bool IsRunning();
void InstallStopSignals();          // Ctrl+C / window close -> g_running=false

// ── URL query-parameter extraction ("/api/x?start=3&count=8", key=start -> "3") ──
std::string QueryParam(const std::string& path, const std::string& key);

// ── WebApi extension-route handlers (shared by both processes; method validation is inside each handler) ──

/// /api/sim/* simulation data plane: status list / registers read (GET) write (POST)
std::pair<int, std::string> HandleSimApi(const HttpRequest& req,
                                         const SimServerMap& sims);

/// latest snapshot -> JSON ({count, tags:[...]}) - shared by /latest and the SSE /stream
std::string BuildLatestJson(const Polling::LatestValueStore& latestStore,
                            const std::string& deviceFilter);

/// GET /api/data/latest real-time data snapshot (?device= filter)
std::pair<int, std::string> HandleDataApi(
    const HttpRequest& req, const Polling::LatestValueStore& latestStore);

/// POST /api/data/write single-register write (full chain via io.post, serialized with the polling callback)
std::pair<int, std::string> HandleWriteApi(const AppContext& ctx,
                                           const HttpRequest& req);

/// GET/POST /api/validate?scope=&name= read-only validation (no disk write/no reload), returning a structured issue list.
/// Implementation see ValidateApi.cpp; scope: protocols|tags.
std::pair<int, std::string> HandleValidateApi(const Service::ConfigStore& store,
                                              const HttpRequest& req);

// ── reload runtime linkage - the sole code path for first-time assembly and hot reload ──
// Reload the config directory -> fully stop and rebuild simulators -> clear real-time snapshots -> post to the io thread
// to re-assemble the engine. On invalid new config, keep the old running state and return an error (pairs with ConfigStore auto-rollback).
Core::VoidExpected ApplyRuntimeSync(const std::string& configDir,
                                    AppContext& ctx, std::ostream* log);

}} // namespace MyProt::App

#endif // MYPROT_APP_RUNTIMEGLUE_HPP

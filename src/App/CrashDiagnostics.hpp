// src/App/CrashDiagnostics.hpp - crash forensics (terminate / SIGABRT / SEH + dbghelp stack trace)
// Plan-1-S1: moved verbatim from RuntimeGlue.cpp (zero behavior change).

#ifndef MYPROT_APP_CRASHDIAGNOSTICS_HPP
#define MYPROT_APP_CRASHDIAGNOSTICS_HPP

namespace MyProt { namespace App {

/// Install crash forensics: set_terminate / SIGABRT / SEH unhandled exception + pre-warm dbghelp symbols
void InstallCrashDiagnostics();

}} // namespace MyProt::App

#endif // MYPROT_APP_CRASHDIAGNOSTICS_HPP

// src/App/AppSignals.hpp - process stop signal and running flag
// Plan-1-S1: moved verbatim from RuntimeGlue.cpp (zero behavior change); RuntimeGlue.hpp still forward-declares, main.cpp needs no change.

#ifndef MYPROT_APP_APPSIGNALS_HPP
#define MYPROT_APP_APPSIGNALS_HPP

namespace MyProt { namespace App {

/// Whether the process should keep running (Ctrl+C / window close -> false)
bool IsRunning();

/// Install SIGINT/SIGTERM handling (Ctrl+C / window close)
void InstallStopSignals();

}} // namespace MyProt::App

#endif // MYPROT_APP_APPSIGNALS_HPP

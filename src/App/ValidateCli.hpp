// src/App/ValidateCli.hpp
// One-shot config validation for the CLI (`--validate <file.json|dir> [--json]`).
//
// Reuses the same load-time validators as the gateway / the /api/validate endpoint,
// so "passes here" means "passes startup". Output carries an RFC 6901 JSON Pointer
// per issue (when the message carries locate information) for editor / CI click-through.
#pragma once

#include <string>

namespace MyProt { namespace App {

/// Validate a protocol file, a tags.json, or a whole config directory.
/// @return 0 = no errors (warnings allowed); 1 = validation errors; 2 = usage / IO error
int RunValidate(const std::string& target, bool jsonOut);

}} // namespace MyProt::App

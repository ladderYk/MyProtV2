// src/Service/include/MyProt/Service/ConfigDirectoryLoader.hpp
// Directory-layout loader - the config entry for production startup mode (Config_Schema §1)

#pragma once
#include <string>
#include <vector>
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // LoadedConfig.server
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Service {

/// ── Config-directory layout contract names (single source of truth for disk paths) ──
///   Shared by ConfigStore and ConfigDirectoryLoader - avoiding writing the same set of dir/file names in two places.
///   The separator is uniformly '/': Win32 and the MSVC CRT _findfirst / ifstream all accept forward slash,
///   the separator is unified to '/' + wildcards (no '\' form may remain).
namespace ConfigLayout {
const char* const kProtocolsDirPath = "/protocols";    // protocol-file directory (with leading separator)
const char* const kTagsFilePath     = "/tags.json";    // devices + tags + resilience + management plane
const char* const kServerFilePath   = "/server.json";  // global server (simulation) config
}

/// Directory-load product: protocol set + config root + server (all having passed deep validation and parsing)
struct LoadedConfig {
    std::vector<Core::ProtocolConfig> protocols;
    Core::ConfigRoot root;
    // Global server config (simulation + future alertSink/webhook). The protocol layer carries no server-side behavior.
    // server.json may be optionally absent - SimulationConfig fields all default (listenPort=0 means simulation off).
    Core::ServerConfig server;

    LoadedConfig() {}
};

/// Directory-layout loader (Config_Schema §1):
///   <dir>/protocols/*.json - the protocol file set (one protocol per file)
///   <dir>/tags.json        - devices + tags + global resilience + WebApi
///   <dir>/server.json      - global server (simulation) config; absent = off by default
///
/// Validation (fifteen rules + cross-file reference) and parsing are combined (single JSON parse):
/// each file calls validateAndParse* - deep validation (Fail-Fast, errors carry the file name)
/// and POCO conversion in one pass; the root-config stage reuses the parsed protocol set, not parsing again.
/// Validation-time Warnings go to stderr and do not block loading.
class ConfigDirectoryLoader {
public:
    /// @param configDir              the config directory
    /// @param supportedSchemaVersion the version-gate generation (ADR-0005)
    static Core::Expected<LoadedConfig> Load(const std::string& configDir,
                                             int supportedSchemaVersion);
};

}} // namespace MyProt::Service

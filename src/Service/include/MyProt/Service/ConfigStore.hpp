// src/Service/include/MyProt/Service/ConfigStore.hpp
// ConfigStore - config read/write service (the config-UI M1 storage backend, see design-proposal/v2-config-ui-design)
// Responsibility: reading / validating / atomic writing / backing up / rolling back JSON config files, triggering a hot-reload callback after save.
// Constraints: pure C++11 / VS2015 (ADR-0010 §3); error handling uniformly goes through Core::Expected.

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"   // kSupportedSchemaVersion

namespace MyProt { namespace Service {

/// Config scope - corresponds to the config-file layout:
///   configDir/protocols/{name}.json   (protocol-level, ConfigScope::Protocol)
///   configDir/tags.json               (devices + tags + resilience + management plane, ConfigScope::Tags)
enum class ConfigScope { Protocol, Tags };

/// ConfigStore construction options
struct ConfigStoreOptions {
    std::string configDir;          // config-file root directory (contains protocols/ and tags.json)
    int         supportedSchemaVersion;  // the currently supported config generation (ADR-0005 version gate)
    int         backupRetention;    // number of backups retained (ring buffer)

    ConfigStoreOptions()
        : configDir("configs"), supportedSchemaVersion(Core::kSupportedSchemaVersion)
        , backupRetention(3) {}
};

/// Config read/write service - the storage backend for the config UI M1.
/// Full pipeline: read / validate / atomic write / backup / rollback.
/// Validation runs the Config_Schema §7 deep rules via the ConfigValidator JSON entry point;
/// this class operates at the JSON-text boundary, doing no POCO conversion (parsing belongs to ConfigDirectoryLoader).
class ConfigStore {
public:
    /// Hot-reload callback - registered by ConfigReloadService; called after a successful Save.
    using ReloadHandler = std::function<Core::VoidExpected()>;

    explicit ConfigStore(const ConfigStoreOptions& options);

    // ── Read ──────────────────────────────────────────────
    /// List all config-item names under a scope (Protocol -> protocol file names; Tags -> ["tags"]).
    Core::Expected<std::vector<std::string>> List(ConfigScope scope) const;

    /// Read a config item, returning the raw JSON text.
    Core::Expected<std::string> Get(ConfigScope scope, const std::string& name) const;

    // ── Validate ────────────────────────────────────────
    /// Validate a config: JSON syntax + schemaVersion gate + Config_Schema §7 deep field validation
    /// (Protocol -> validateProtocolJson; Tags -> validateConfigRootJson,
    ///  additionally passing the full current protocols/*.json for cross-file reference validation).
    /// Returns the full error list (Warnings carry a "[WARN] " prefix); empty = pass.
    Core::Expected<std::vector<std::string>> Validate(ConfigScope scope,
                                                      const std::string& name,
                                                      const std::string& payload) const;

    // ── Write ──────────────────────────────────────────────
    /// Save a config: validate -> back up the current version -> atomic write -> trigger a hot reload.
    /// A validation failure does not write to disk; a reload failure auto-rolls back and returns an error.
    Core::Expected<void> Save(ConfigScope scope, const std::string& name, const std::string& payload);

    // ── Backup / rollback ─────────────────────────────────
    /// List the rollback-able backup tags of a config item (e.g. "bak.1", "bak.2").
    Core::Expected<std::vector<std::string>> ListBackups(ConfigScope scope,
                                                         const std::string& name) const;

    /// Roll back to a specified backup tag.
    Core::Expected<void> Rollback(ConfigScope scope, const std::string& name,
                                  const std::string& backupTag);

    // ── Delete ────────────────────────────────────────────
    /// Delete a config item and all its backups (Protocol only; Tags is a fixed single file and does not support deletion).
    Core::Expected<void> Delete(ConfigScope scope, const std::string& name);

    // ── Dependency injection ────────────────────────────
    /// Register the hot-reload callback (injected by ConfigReloadService during assembly).
    void SetReloadHandler(ReloadHandler handler);

    /// Manually trigger a hot reload (the WebApi /api/config/reload endpoint); succeeds directly when no callback is registered.
    Core::Expected<void> Reload();

    /// The currently supported config generation (ADR-0005); used by the GET /api/config/schema endpoint for delivery.
    int SupportedSchemaVersion() const { return _opts.supportedSchemaVersion; }

private:
    // Parse and validate a config item's path; guard against path traversal (only legal file names allowed).
    std::string ResolvePath(ConfigScope scope, const std::string& name,
                            Core::Error* err) const;
    bool IsValidName(const std::string& name) const;

    Core::Expected<std::string> ReadFile(const std::string& path) const;
    Core::Expected<void> AtomicWrite(const std::string& path, const std::string& content);
    Core::Expected<void> BackupFile(const std::string& path);
    Core::Expected<void> TriggerReload();

    /// Lock-free internal implementation of Rollback - the caller must already hold _mutex;
    /// reused by Save's auto-rollback path (the public Rollback locks then delegates, avoiding same-thread re-entrant deadlock).
    Core::Expected<void> RollbackUnlocked(ConfigScope scope, const std::string& name,
                                          const std::string& backupTag);

    ConfigStoreOptions _opts;
    ReloadHandler _reload;      // empty when unregistered; Save skips reload
    mutable std::mutex _mutex;  // serializes write operations, avoiding races with reload
};

}} // namespace MyProt::Service
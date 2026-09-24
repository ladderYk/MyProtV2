// src/Service/include/MyProt/Service/ConfigValidator.hpp
// Config deep validator - Config_Schema §7 fifteen rules + version gate + resilience/management-plane validation
// The main API operates on POCO structs; ValidateJson* / validateAndParse* are JSON-text entry points.

#pragma once
#include <string>
#include <vector>
#include <set>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Service {

/// Validation result: errors (block startup) and warnings (do not block, logged)
struct ValidationResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool hasErrors() const { return !errors.empty(); }

    void addError(const std::string& msg)   { errors.push_back(msg); }
    void addWarning(const std::string& msg) { warnings.push_back(msg); }

    /// Merge another result (warnings merged; errors merged, then if the other had errors the whole is hasErrors)
    void merge(const ValidationResult& other) {
        errors.insert(errors.end(), other.errors.begin(), other.errors.end());
        warnings.insert(warnings.end(), other.warnings.begin(), other.warnings.end());
    }
};

/// Combined validate + parse product: a single JSON parse yields both the deep-validation result and the POCO
/// (for ConfigDirectoryLoader's production load path, eliminating the validate+parse double parse)
struct ProtocolValidationOutcome {
    ValidationResult result;
    Core::ProtocolConfig proto;
};

struct ConfigRootValidationOutcome {
    ValidationResult result;
    Core::ConfigRoot root;
};

class ConfigValidator {
public:
    explicit ConfigValidator(int supportedSchemaVersion = 2);

    // ── POCO entry points ──

    /// Validate a single protocol (corresponds to protocols/*.json)
    ValidationResult validateProtocol(const Core::ProtocolConfig& proto) const;

    /// Validate the config root (corresponds to tags.json); if a protocol list is passed, do cross-file reference validation
    ValidationResult validateConfigRoot(
        const Core::ConfigRoot& root,
        const std::vector<Core::ProtocolConfig>& protocols = std::vector<Core::ProtocolConfig>()) const;

    // ── JSON-text entry points (called directly by ConfigStore) ──

    /// Validate protocol JSON text
    static Core::Expected<ValidationResult> validateProtocolJson(
        const std::string& json, int supportedSchemaVersion = 2);

    /// Validate the tag-root JSON text (optionally with a protocol JSON list for cross-file reference)
    static Core::Expected<ValidationResult> validateConfigRootJson(
        const std::string& json,
        const std::vector<std::string>& protocolJsons = std::vector<std::string>(),
        int supportedSchemaVersion = 2);

    // ── JSON -> POCO parsing (for ConfigDirectoryLoader's production load path) ──
    static Core::Expected<Core::ProtocolConfig> parseProtocolJson(const std::string& json);
    static Core::Expected<Core::ConfigRoot> parseConfigRootJson(const std::string& json);

    // ── Trial validation (ADR-0012 §2, implemented in FrameConsistencyCheck.cpp) ──
    /// For a protocol's LengthField write operations containing {Name:raw} payload placeholders, trial-render one frame with the real pipeline:
    ///   1) derived-length expr resolvability (evaluation failure -> error, sealing off a silent runtime 0 value);
    ///   2) consistency between the rendered frame and the framing length slot (a three-way cross-check of template fixed segment <-> outputs constant <-> framing);
    ///   3) interception of the {Frame:fixed} reserved name / unknown-format placeholders.
    /// Input is an already-parsed POCO. Attachment points: save-time ConfigStore::Validate +
    /// load-time vN validation entry (validateConfigRootJson / validateAndParseConfigRootJson).
    /// @return the error list (empty = pass)
    static std::vector<std::string> CheckFrameConsistency(
        const Core::ProtocolConfig& proto);

    // ── Grammar utilities (reused by other TUs in the same library, e.g. ConfigDirectoryLoader's simulation-template-line validation) ──
    /// Hex-literal line validation: each byte is exactly two hex digits, space/tab separated, at least one token.
    static bool isValidHexLiteral(const std::string& s);

    // ── Combined validate + parse entry (single JSON parse; for the production load path) ──

    /// Protocol JSON: deep-validate and parse into a POCO
    static Core::Expected<ProtocolValidationOutcome> validateAndParseProtocolJson(
        const std::string& json, int supportedSchemaVersion = 2);

    /// Config-root JSON: deep-validate and parse into a POCO.
    /// protocols is the protocol set already each validated - only root-level rules and cross-file reference validation run,
    /// without repeating per-protocol parsing/validation (the caller completes it once during the protocol-load stage).
    static Core::Expected<ConfigRootValidationOutcome> validateAndParseConfigRootJson(
        const std::string& json,
        const std::vector<Core::ProtocolConfig>& protocols,
        int supportedSchemaVersion = 2);

private:
    int _supportedVersion;

    // ── Version gate (the only check that terminates early) ──
    bool checkVersionGate(int protoVersion, ValidationResult& r) const;

    // ── Protocol layer (§7 rules 1-8) ──
    void validateTransport(const Core::TransportConfig& t, ValidationResult& r) const;
    void validateFraming(const Core::FramingConfig& f,
                         const Core::TransportConfig& t, ValidationResult& r) const;
    void validateHandshake(const std::vector<Core::HandshakeStep>& hs,
                           const Core::TransportConfig& transport,
                           ValidationResult& r) const;

    // ── Template grammar (§3) ──
    void validateTemplate(const std::string& opName, const std::string& line,
                          ValidationResult& r) const;
    /// Derived-length offset self-check - validates that a derived variable sitting in the framing length slot has a constant offset relative to payload consistent with the template layout
    void ValidateDerivedLengthOffsets(const Core::ProtocolConfig& proto,
                                      const Core::OperationConfig& op,
                                      const std::string& opName,
                                      ValidationResult& r) const;

    // ── Device layer (§7 rules 9-11) ──
    void validateDevice(const Core::DeviceConfig& dev,
                        const std::unordered_map<std::string,
                            const Core::ProtocolConfig*>& protoMap,
                        ValidationResult& r) const;

    // ── Tag layer (§7 rules 12-15) ──
    void validateTag(const Core::TagDefinition& tag,
                     const std::unordered_map<std::string,
                         const Core::DeviceConfig*>& deviceMap,
                     const std::unordered_map<std::string,
                         const Core::ProtocolConfig*>& protoMap,
                     ValidationResult& r) const;

    // ── Resilience policy (§11) ──
    void validateResilience(const Core::ResilienceConfig& res,
                            const std::string& ctx, ValidationResult& r) const;

    // ── Management plane (§12) ──
    void validateWebApi(const Core::WebApiConfig& api, ValidationResult& r) const;

    // ── Utility methods ──
    static bool isValidPlaceholder(const std::string& s);
    static bool isValidIPAddress(const std::string& s);
    static bool isNumericFinalType(const std::string& ft);
    static bool isAllowedFinalType(const std::string& ft);
};

}} // namespace MyProt::Service

// src/Service/include/MyProt/Service/ValidationIssue.hpp
// Structuring of validation results - maps ValidationResult's string entries into machine-readable "issue" objects,
// for the /api/validate read-only endpoint and the management plane (webui) to do categorized rendering / click-to-locate.
//
// Why classify at the boundary instead of rewriting the validator:
//   ConfigDeepValidator has ~60 sites of r.addError("..."); changing each into a struct is a large edit with no regression net;
//   yet all messages are in stable formats ("operation X ..." / "device X ..." / "tag X ..."), so one rule table doing
//   substring matching suffices to obtain (ruleId, subject, suggested locate path).
//   Key constraint: **only add information, never lose any** - entries unmatched by the rule table are still returned verbatim
//   (ruleId="unclassified"), and message is always the validator's original text.
//
// Layering: this header belongs to the Service layer, depending only on <string>/<vector> (no nlohmann);
//   JSON serialization is done by the WebApi/App layer (see src/App/ValidateApi.cpp).
#pragma once

#include <string>
#include <vector>

namespace MyProt { namespace Service {

/// A single validation issue - severity=error blocks saving; warning is only a hint (formerly the "[WARN] "-prefixed entries)
struct ValidationIssue {
    std::string severity;   // "error" | "warning"
    std::string ruleId;     // rule identifier, e.g. "frame.length_consistency"; unclassified = "unclassified"
    std::string scope;      // validation domain: "protocol" | "tags" (passed in by the caller)
    std::string subject;    // the located subject: operation name / device ID / tag name (may be empty)
    std::string field;      // suggested locate path, e.g. "operations.WriteRegisters.requestTemplate" (may be empty)
    std::string message;    // the validator's original message (with the "[WARN] " prefix stripped)
};

/// Classify ConfigStore::Validate's entry list into structured issues.
/// - severity: determined by the "[WARN] " prefix
/// - ruleId/subject/field: matched against an internal rule table; no match -> "unclassified" + empty subject/field
/// - order matches the input, entry count never decreases (no entry is dropped)
std::vector<ValidationIssue> ClassifyValidationItems(
    const std::vector<std::string>& items, const std::string& scope);

}} // namespace MyProt::Service

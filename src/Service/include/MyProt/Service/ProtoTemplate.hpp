// src/Service/include/MyProt/Service/ProtoTemplate.hpp
// Built-in minimal protocol templates - the P0 on-boarding scaffold.
//
// Single source of truth for `MyProt.App --proto-init <kind>`. The same text is
// parsed and validated by Service.Tests, so a template that stops passing the
// config validator breaks CI instead of silently handing users a broken file.

#pragma once

#include <string>
#include <vector>

namespace MyProt { namespace Service {

/// One scaffolder entry.
struct ProtoTemplate {
    std::string kind;          // CLI selector (e.g. "lengthfield")
    std::string protocolName;  // default protocolName written into the file
    std::string exampleOperation; // operation a first tag should reference
    std::string json;          // complete protocol JSON text (UTF-8, LF)
    std::string summary;       // one-line help text

    ProtoTemplate() {}
};

/// All templates in help order (no duplicates).
const std::vector<ProtoTemplate>& ListProtoTemplates();

/// Find by kind (case-sensitive); null when unknown.
const ProtoTemplate* FindProtoTemplate(const std::string& kind);

}} // namespace MyProt::Service

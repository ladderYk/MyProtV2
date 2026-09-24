// src/Service/include/MyProt/Service/SchemaRegistry.hpp
// FieldDescriptor registry (M2) - the data source of GET /api/config/schema,
// for the Vue management UI to render dynamic forms; its content stays consistent with the docs/Config_Schema.md contract.

#pragma once
#include <string>
#include <vector>

namespace MyProt { namespace Service {

/// Field descriptor - describes the structural constraints of a single field in the config JSON
struct FieldDescriptor {
    std::string name;                        // field name
    std::string type;                        // string/int/uint8/uint16/uint32/bool/double/
                                             // enum/object/array/array<string>/array<object>/map
    bool required;                           // whether it is required
    std::string defaultValue;                // default-value text; empty = no default note
    std::string description;                 // semantic description (incl. validation-bound hints)
    std::vector<std::string> enumValues;     // valid when type == "enum"
    std::vector<FieldDescriptor> children;   // valid when type == "object"/"array<object>"

    FieldDescriptor() : required(false) {}
};

class SchemaRegistry {
public:
    /// tags.json root-level fields (schemaVersion/resilience/webApi/devices/tags)
    static const std::vector<FieldDescriptor>& RootFields();
    /// Protocol JSON root-level fields (transport/framing/operations/handshake...; excluding builtInFunctions)
    static const std::vector<FieldDescriptor>& ProtocolFields();
    /// devices[] entry fields
    static const std::vector<FieldDescriptor>& DeviceFields();
    /// tags[] entry fields
    static const std::vector<FieldDescriptor>& TagFields();

    /// Serialize the whole registry:
    /// {"schemaVersion":N,"sections":{"root":[...],"protocol":[...],"device":[...],"tag":[...]}}
    static std::string ToJson(int supportedSchemaVersion);
};

}} // namespace MyProt::Service

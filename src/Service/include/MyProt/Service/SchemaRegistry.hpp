// src/Service/include/MyProt/Service/SchemaRegistry.hpp
// FieldDescriptor 字段注册表 (M2) — GET /api/config/schema 数据源,
// 供 Vue 管理界面动态表单渲染; 表内容与 docs/Config_Schema.md 契约保持一致。

#pragma once
#include <string>
#include <vector>

namespace MyProt { namespace Service {

/// 字段描述符 — 描述配置 JSON 中单个字段的结构约束
struct FieldDescriptor {
    std::string name;                        // 字段名
    std::string type;                        // string/int/uint8/uint16/uint32/bool/double/
                                             // enum/object/array/array<string>/array<object>/map
    bool required;                           // 是否必填
    std::string defaultValue;                // 缺省值文本; 空 = 无缺省说明
    std::string description;                 // 语义说明 (含校验边界提示)
    std::vector<std::string> enumValues;     // type == "enum" 时有效
    std::vector<FieldDescriptor> children;   // type == "object"/"array<object>" 时有效

    FieldDescriptor() : required(false) {}
};

class SchemaRegistry {
public:
    /// tags.json 根层字段 (schemaVersion/resilience/webApi/devices/tags)
    static const std::vector<FieldDescriptor>& RootFields();
    /// 协议 JSON 根层字段 (transport/framing/operations/handshake...; 不含 builtInFunctions)
    static const std::vector<FieldDescriptor>& ProtocolFields();
    /// devices[] 条目字段
    static const std::vector<FieldDescriptor>& DeviceFields();
    /// tags[] 条目字段
    static const std::vector<FieldDescriptor>& TagFields();

    /// 序列化整个注册表:
    /// {"schemaVersion":N,"sections":{"root":[...],"protocol":[...],"device":[...],"tag":[...]}}
    static std::string ToJson(int supportedSchemaVersion);
};

}} // namespace MyProt::Service

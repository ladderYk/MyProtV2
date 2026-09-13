// src/Engine/include/MyProt/Engine/ResponseParser.hpp
// 响应解析器 — 数据提取与质量评估 (C++11, ADR-0010 §5)

#pragma once
#include <string>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Engine {

// ────────── 具体解析器 — 单标签响应解析 (E2E/Gateway 直用) ──────────

/// 解析流程:
///   1. validCondition 校验 (支持 "resp[N]==V", V 为十进制或 0x 十六进制)
///   2. dataStartIndex 定位数据区
///   3. registerCount×2 字节 (不足则取剩余全量) 按 finalType 转换
///   4. 产出 TagValue (quality=Good, timestamp=now)
class ResponseParser {
public:
    /// 字节序裁决: tag 级覆盖 → 协议 dataByteOrder → 默认大端
    /// (单读/批量读等所有管线的唯一裁决点, 避免各处重复三段式回退逻辑)
    static Core::ByteOrder ResolveByteOrder(
        const Core::ProtocolConfig& protocol,
        const Core::Optional<Core::ByteOrder>& tagOverride);

    /// 校验 validCondition 子集 "resp[N]==V"
    /// (写应答 echo 等只需条件校验、不解析数据区的场景)
    static bool CheckCondition(const Core::ByteView& response,
                               const std::string& validCondition);

    /// 解析响应并转换为标签值
    /// @param response 响应字节流
    /// @param config 解析器配置 (validCondition/dataStartIndex 等)
    /// @param tag 标签定义 (finalType/registerCount/name/deviceId)
    /// @param byteOrder 字节序
    /// @return 标签值; 失败返回 InvalidResponse/ParseError/TypeConversionError
    Core::Expected<Core::TagValue> Parse(
        const Core::ByteView& response,
        const Core::ResponseParserConfig& config,
        const Core::TagDefinition& tag,
        Core::ByteOrder byteOrder);
};

}} // namespace MyProt::Engine

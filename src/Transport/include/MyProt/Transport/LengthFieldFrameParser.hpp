// src/Transport/include/MyProt/Transport/LengthFieldFrameParser.hpp
// LengthField 帧解析器 — 支持可变长度字段协议 (C++11, ADR-0010 §4)

#pragma once
#include "IFrameParser.hpp"
#include "MyProt/Core/Config.hpp"
#include <vector>

namespace MyProt { namespace Transport {

/// LengthField 帧解析器实现
class LengthFieldFrameParser : public IFrameParser {
public:
    explicit LengthFieldFrameParser(const Core::LengthFieldConfig& config);
    virtual ~LengthFieldFrameParser() = default;

    // IFrameParser 接口实现
    Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) override;
    bool HasCompleteFrame(const ByteView& data) override;

private:
    /// 配置
    Core::LengthFieldConfig _config;

    /// 私有方法
    Expected<size_t> ExtractLengthValue(const ByteView& headerData);
    size_t CalculateTotalFrameSize(size_t bodyLength) const;
    bool ValidateFrameSize(size_t totalSize) const;
    
    // 字节序转换
    uint16_t ToUInt16BigEndian(const uint8_t* data) const;
    uint16_t ToUInt16LittleEndian(const uint8_t* data) const;
    uint32_t ToUInt32BigEndian(const uint8_t* data) const;
    uint32_t ToUInt32LittleEndian(const uint8_t* data) const;
};

}} // namespace MyProt::Transport
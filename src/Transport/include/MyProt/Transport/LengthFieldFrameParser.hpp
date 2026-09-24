// src/Transport/include/MyProt/Transport/LengthFieldFrameParser.hpp
// LengthField frame parser - supports variable-length-field protocols (C++11, ADR-0010 §4)

#pragma once
#include "IFrameParser.hpp"
#include "MyProt/Core/Config.hpp"
#include <vector>

namespace MyProt { namespace Transport {

/// LengthField frame-parser implementation
class LengthFieldFrameParser : public IFrameParser {
public:
    explicit LengthFieldFrameParser(const Core::LengthFieldConfig& config);
    virtual ~LengthFieldFrameParser() = default;

    // IFrameParser interface implementation
    Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) override;
    bool HasCompleteFrame(const ByteView& data) override;

private:
    /// Configuration
    Core::LengthFieldConfig _config;

    /// Private methods
    Expected<size_t> ExtractLengthValue(const ByteView& headerData);
    size_t CalculateTotalFrameSize(size_t bodyLength) const;
    bool ValidateFrameSize(size_t totalSize) const;
    
    // Byte-order conversion
    uint16_t ToUInt16BigEndian(const uint8_t* data) const;
    uint16_t ToUInt16LittleEndian(const uint8_t* data) const;
    uint32_t ToUInt32BigEndian(const uint8_t* data) const;
    uint32_t ToUInt32LittleEndian(const uint8_t* data) const;
};

}} // namespace MyProt::Transport
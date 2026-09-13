// src/Transport/src/LengthFieldFrameParser.cpp
// LengthField 帧解析器实现 — 支持可变长度字段协议 (C++11, ADR-0010 §4)

#include "MyProt/Transport/LengthFieldFrameParser.hpp"
#include <algorithm>
#include <cstdint>

namespace MyProt { namespace Transport {

LengthFieldFrameParser::LengthFieldFrameParser(const Core::LengthFieldConfig& config)
    : _config(config) {
}

Expected<FrameParseResult> LengthFieldFrameParser::Parse(const ByteView& data, size_t startPos) {
    if (startPos > data.size) {
        return Unexpected(Error::Code::ParseError, "Invalid start position");
    }

    size_t available = data.size - startPos;
    if (available == 0) {
        FrameParseResult result;
        result.needMoreData = true;
        result.consumedBytes = 0;
        return result;
    }

    // 检查是否有足够的头部数据来提取长度字段
    size_t headerRequired = _config.lengthFieldOffset + _config.lengthFieldLength;
    if (available < headerRequired) {
        FrameParseResult result;
        result.needMoreData = true;
        result.consumedBytes = available;
        return result;
    }

    // 提取长度值
    ByteView headerData;
    headerData.data = data.data + startPos;
    headerData.size = data.size - startPos;
    auto lengthResult = ExtractLengthValue(headerData);
    if (!lengthResult.has_value()) {
        const Error& err = lengthResult.error();
        return Unexpected(err.code, err.message);
    }

    size_t lengthValue = lengthResult.value();
    size_t totalFrameSize = CalculateTotalFrameSize(lengthValue);

    // 检查帧大小是否有效
    if (!ValidateFrameSize(totalFrameSize)) {
        return Unexpected(Error::Code::ParseError, "Frame size exceeds maximum limit");
    }

    // 检查是否有完整的帧数据
    if (available < totalFrameSize) {
        FrameParseResult result;
        result.needMoreData = true;
        result.consumedBytes = available;
        return result;
    }

    // 提取完整帧
    FrameParseResult result;
    result.frame.assign(data.data + startPos, data.data + startPos + totalFrameSize);
    result.consumedBytes = totalFrameSize;
    result.needMoreData = false;

    return result;
}

bool LengthFieldFrameParser::HasCompleteFrame(const ByteView& data) {
    if (data.empty()) {
        return false;
    }

    auto parseResult = Parse(data, 0);
    return parseResult.has_value() && !parseResult.value().needMoreData;
}

// 私有方法实现

Expected<size_t> LengthFieldFrameParser::ExtractLengthValue(const ByteView& headerData) {
    if (headerData.size < _config.lengthFieldOffset + _config.lengthFieldLength) {
        return Unexpected(Error::Code::ParseError, "Insufficient header data for length extraction");
    }

    const uint8_t* lengthData = headerData.data + _config.lengthFieldOffset;

    switch (_config.lengthFieldLength) {
        case 1:
            return static_cast<size_t>(lengthData[0]);

        case 2: {
            uint16_t length;
            if (_config.byteOrder == Core::ByteOrder::BigEndian) {
                length = ToUInt16BigEndian(lengthData);
            } else {
                length = ToUInt16LittleEndian(lengthData);
            }
            return static_cast<size_t>(length);
        }

        case 4: {
            uint32_t length;
            if (_config.byteOrder == Core::ByteOrder::BigEndian) {
                length = ToUInt32BigEndian(lengthData);
            } else {
                length = ToUInt32LittleEndian(lengthData);
            }
            return static_cast<size_t>(length);
        }

        default:
            return Unexpected(Error::Code::ConfigError, "Unsupported length field length");
    }
}

size_t LengthFieldFrameParser::CalculateTotalFrameSize(size_t bodyLength) const {
    if (_config.lengthIncludesHeader) {
        // length 字段值包含帧头长度
        return bodyLength + _config.lengthAdjustment;
    } else {
        // length 字段值不包含帧头长度
        return _config.headerLength + bodyLength + _config.lengthAdjustment;
    }
}

bool LengthFieldFrameParser::ValidateFrameSize(size_t totalSize) const {
    return totalSize <= _config.maxFrameSize && totalSize > 0;
}

// 字节序转换实现

uint16_t LengthFieldFrameParser::ToUInt16BigEndian(const uint8_t* data) const {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint16_t LengthFieldFrameParser::ToUInt16LittleEndian(const uint8_t* data) const {
    return data[0] | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t LengthFieldFrameParser::ToUInt32BigEndian(const uint8_t* data) const {
    return (static_cast<uint32_t>(data[0]) << 24) | 
           (static_cast<uint32_t>(data[1]) << 16) | 
           (static_cast<uint32_t>(data[2]) << 8) | 
           data[3];
}

uint32_t LengthFieldFrameParser::ToUInt32LittleEndian(const uint8_t* data) const {
    return data[0] | 
           (static_cast<uint32_t>(data[1]) << 8) | 
           (static_cast<uint32_t>(data[2]) << 16) | 
           (static_cast<uint32_t>(data[3]) << 24);
}

}} // namespace MyProt::Transport
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

    const size_t available = data.size - startPos;
    FrameParseResult result;
    if (available == 0) {
        result.needMoreData = true;
        result.consumedBytes = 0;
        return result;
    }

    const FrameLayout layout = Measure(_config, data.data + startPos, available);
    if (layout.malformed) {
        return Unexpected(Error::Code::ParseError, layout.error);
    }
    if (layout.needMore) {
        result.needMoreData = true;
        result.consumedBytes = available;
        return result;
    }

    result.frame.assign(data.data + startPos,
                        data.data + startPos + layout.totalBytes);
    result.consumedBytes = layout.totalBytes;
    result.needMoreData = false;
    return result;
}

LengthFieldFrameParser::FrameLayout LengthFieldFrameParser::Measure(
        const Core::LengthFieldConfig& config, const uint8_t* frame,
        size_t available) {
    FrameLayout layout;

    // 检查是否有足够的数据开始读取长度域。Varint 宽度不定, 至少要 1 字节。
    const size_t lengthWidth = config.lengthEncoding == Core::LengthFieldEncoding::Varint
        ? 1 : static_cast<size_t>(config.lengthFieldLength);
    const size_t headerRequired = static_cast<size_t>(config.lengthFieldOffset) + lengthWidth;
    if (available < headerRequired) {
        layout.needMore = true;
        return layout;
    }

    // 提取长度值 (同时得到编码宽度: Fixed = lengthFieldLength, Varint = 实际字节数)
    size_t lengthValue = 0;
    size_t encodedWidth = 0;
    std::string err;
    const LengthStatus st = ReadLength(config, frame, available,
                                       lengthValue, encodedWidth, err);
    if (st == LengthStatus::NeedMore) {
        layout.needMore = true;
        return layout;
    }
    if (st == LengthStatus::Malformed) {
        layout.malformed = true;
        layout.error = err;
        return layout;
    }

    const int64_t totalFrameSize = CalculateTotalFrameSize(config, lengthValue, encodedWidth);
    if (!ValidateFrameSize(config, totalFrameSize)) {
        layout.malformed = true;
        layout.error = "Frame size exceeds maximum limit";
        return layout;
    }

    if (available < static_cast<size_t>(totalFrameSize)) {
        layout.needMore = true;
        return layout;
    }

    layout.totalBytes = static_cast<size_t>(totalFrameSize);
    return layout;
}

bool LengthFieldFrameParser::HasCompleteFrame(const ByteView& data) {
    if (data.empty()) {
        return false;
    }

    auto parseResult = Parse(data, 0);
    return parseResult.has_value() && !parseResult.value().needMoreData;
}

// 私有方法实现

LengthFieldFrameParser::LengthStatus LengthFieldFrameParser::ReadLength(
        const Core::LengthFieldConfig& config, const uint8_t* frame,
        size_t available, size_t& value, size_t& width, std::string& err) {
    const size_t offset = static_cast<size_t>(config.lengthFieldOffset);
    if (available < offset + 1) {
        err = "Insufficient header data for length extraction";
        return LengthStatus::NeedMore;
    }

    if (config.lengthEncoding == Core::LengthFieldEncoding::Varint) {
        // base-128: 低 7 位有效, 最高位 = 继续标志 (ADR-0021 §2.1)
        const size_t maxBytes = static_cast<size_t>(config.varintMaxBytes);
        uint64_t decoded = 0;
        for (size_t i = 0; i < maxBytes; ++i) {
            if (offset + i >= available) {
                err = "Incomplete varint length field";
                return LengthStatus::NeedMore;
            }
            const uint8_t b = frame[offset + i];
            decoded |= (static_cast<uint64_t>(b & 0x7F) << (7 * i));
            if ((b & 0x80) == 0) {
                value = static_cast<size_t>(decoded);
                width = i + 1;
                return LengthStatus::Complete;
            }
        }
        err = "Varint length field exceeds varintMaxBytes";
        return LengthStatus::Malformed;
    }

    if (available < offset + static_cast<size_t>(config.lengthFieldLength)) {
        err = "Insufficient header data for length extraction";
        return LengthStatus::NeedMore;
    }

    const uint8_t* lengthData = frame + offset;
    width = static_cast<size_t>(config.lengthFieldLength);
    switch (config.lengthFieldLength) {
        case 1:
            value = static_cast<size_t>(lengthData[0]);
            return LengthStatus::Complete;

        case 2: {
            const uint16_t length = (config.byteOrder == Core::ByteOrder::BigEndian)
                ? ToUInt16BigEndian(lengthData) : ToUInt16LittleEndian(lengthData);
            value = static_cast<size_t>(length);
            return LengthStatus::Complete;
        }

        case 4: {
            const uint32_t length = (config.byteOrder == Core::ByteOrder::BigEndian)
                ? ToUInt32BigEndian(lengthData) : ToUInt32LittleEndian(lengthData);
            value = static_cast<size_t>(length);
            return LengthStatus::Complete;
        }

        default:
            err = "Unsupported length field length";
            return LengthStatus::Malformed;
    }
}

int64_t LengthFieldFrameParser::CalculateTotalFrameSize(
        const Core::LengthFieldConfig& config, size_t bodyLength,
        size_t lengthWidth) {
    const int64_t body = static_cast<int64_t>(bodyLength);
    const int64_t adj = static_cast<int64_t>(config.lengthAdjustment);
    if (config.lengthIncludesHeader) {
        // length 字段值包含帧头长度
        return body + adj;
    }
    if (config.headerLength > 0) {
        // length 字段值不包含帧头长度, 帧头长度由配置给出
        return static_cast<int64_t>(config.headerLength) + body + adj;
    }
    if (config.lengthEncoding == Core::LengthFieldEncoding::Varint) {
        // headerLength=0 且 Varint: 帧头 = 长度域之前的字节 + 长度域自身 (宽度运行时才知道)
        return static_cast<int64_t>(config.lengthFieldOffset)
             + static_cast<int64_t>(lengthWidth) + body + adj;
    }
    // Fixed 且 headerLength=0: 保持既有语义 (长度值即总帧长, 不叠加帧头)
    return body + adj;
}

bool LengthFieldFrameParser::ValidateFrameSize(const Core::LengthFieldConfig& config,
                                               int64_t totalSize) {
    return totalSize > 0 && totalSize <= config.maxFrameSize;
}

// 字节序转换实现

uint16_t LengthFieldFrameParser::ToUInt16BigEndian(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint16_t LengthFieldFrameParser::ToUInt16LittleEndian(const uint8_t* data) {
    return data[0] | (static_cast<uint16_t>(data[1]) << 8);
}

uint32_t LengthFieldFrameParser::ToUInt32BigEndian(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | 
           (static_cast<uint32_t>(data[1]) << 16) | 
           (static_cast<uint32_t>(data[2]) << 8) | 
           data[3];
}

uint32_t LengthFieldFrameParser::ToUInt32LittleEndian(const uint8_t* data) {
    return data[0] | 
           (static_cast<uint32_t>(data[1]) << 8) | 
           (static_cast<uint32_t>(data[2]) << 16) | 
           (static_cast<uint32_t>(data[3]) << 24);
}

}} // namespace MyProt::Transport
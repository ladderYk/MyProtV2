// src/Transport/include/MyProt/Transport/LengthFieldFrameParser.hpp
// LengthField frame parser - supports variable-length-field protocols (C++11, ADR-0010 §4)

#pragma once
#include "IFrameParser.hpp"
#include "MyProt/Core/Config.hpp"
#include <string>
#include <vector>

namespace MyProt { namespace Transport {

/// LengthField frame-parser implementation
class LengthFieldFrameParser : public IFrameParser {
public:
    /// 帧边界测量结果（不拷贝帧）。Fill 由 Parse 与 TcpChannel::FrameComplete 共用，
    /// 保证长度域语义（定长 / varint）只有一份实现（ADR-0021）。
    struct FrameLayout {
        bool needMore;          // 数据不足，需继续累积
        bool malformed;         // 规格/数据非法
        std::size_t totalBytes; // 完整帧字节数（needMore/malformed 时无意义）
        std::string error;

        FrameLayout() : needMore(false), malformed(false), totalBytes(0) {}
    };

    /// 只测量帧边界；帧从 `data[0]` 开始，`available` 为可读字节数。
    static FrameLayout Measure(const Core::LengthFieldConfig& config,
                               const uint8_t* data, std::size_t available);

    explicit LengthFieldFrameParser(const Core::LengthFieldConfig& config);
    virtual ~LengthFieldFrameParser() = default;

    // IFrameParser interface implementation
    Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) override;
    bool HasCompleteFrame(const ByteView& data) override;

private:
    /// Configuration
    Core::LengthFieldConfig _config;

    /// Outcome of reading the length field itself.
    enum class LengthStatus { Complete, NeedMore, Malformed };

    /// Private methods
    /// Reads the length field at config.lengthFieldOffset. `available` is the byte count
    /// from the start of the frame; returns the decoded value and its encoded width
    /// (which differs from lengthFieldLength only for Varint).
    static LengthStatus ReadLength(const Core::LengthFieldConfig& config,
                                   const uint8_t* frame, size_t available,
                                   size_t& value, size_t& width, std::string& err);
    static int64_t CalculateTotalFrameSize(const Core::LengthFieldConfig& config,
                                           size_t bodyLength, size_t lengthWidth);
    static bool ValidateFrameSize(const Core::LengthFieldConfig& config, int64_t totalSize);
    
    // Byte-order conversion
    static uint16_t ToUInt16BigEndian(const uint8_t* data);
    static uint16_t ToUInt16LittleEndian(const uint8_t* data);
    static uint32_t ToUInt32BigEndian(const uint8_t* data);
    static uint32_t ToUInt32LittleEndian(const uint8_t* data);
};

}} // namespace MyProt::Transport
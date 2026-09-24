// src/Transport/include/MyProt/Transport/IFrameParser.hpp
// Frame-parser interface - extracts complete frames from a byte stream (C++11, ADR-0010 §4)

#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Transport {

// The interface signature uses Core-layer types directly; the using declarations let derived-class implementations avoid re-qualifying
using Core::ByteView;
using Core::Expected;
using Core::Unexpected;
using Core::Error;

/// Frame-parse result
struct FrameParseResult {
    std::vector<uint8_t> frame;      // complete frame data
    size_t consumedBytes;             // number of bytes consumed
    bool needMoreData;               // whether more data is needed
};

/// Frame-parser interface
class IFrameParser {
public:
    virtual ~IFrameParser() = default;

    /// Attempt to parse a frame from the buffer
    /// @param data the input data buffer
    /// @param startPos the position to begin parsing
    /// @return the parse result, containing the frame data and the number of bytes consumed
    virtual Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) = 0;

    /// Check whether a complete frame exists at the head of the buffer
    /// @param data the input data buffer
    /// @return true if a complete frame exists at the head of the buffer
    virtual bool HasCompleteFrame(const ByteView& data) = 0;
};

/// Frame-parser smart pointer
using IFrameParserPtr = std::shared_ptr<IFrameParser>;

}} // namespace MyProt::Transport
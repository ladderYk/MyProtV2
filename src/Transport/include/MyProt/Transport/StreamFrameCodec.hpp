// src/Transport/include/MyProt/Transport/StreamFrameCodec.hpp
// Bottom of the frame stack: byte stream -> Core::Frame (opaque fields, no semantics).
//
// For LengthField framing, Frame::payload starts after the declared header
// (headerLength, or lengthFieldOffset + encoded length width when headerLength = 0), so
// the next layer sees only the application bytes. For Fixed framing the whole frame is
// the payload. Type / named fields are extracted from the raw frame by ApplyFrameSpec.
// This is the slot a data-driven codec plugs into (ADR-0020, ADR-0021); callers only ever
// see Core::IFrameCodec.
//
// Supported framing: LengthField (fixed or varint length, plus optional headerFields /
// dispatch / repeat, ADR-0021) and Fixed. Silence/Message are rejected by the caller before
// construction (they are not implemented for inbound streams in v1).
//
// Lifetime: Frame::payload points into this codec's internal buffer and stays valid until
// the next Decode() call. The codec is not reentrant.

#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/FrameCodec.hpp"
#include "MyProt/Transport/IFrameParser.hpp"

namespace MyProt { namespace Transport {

class StreamFrameCodec : public Core::IFrameCodec {
public:
    explicit StreamFrameCodec(const Core::FramingConfig& cfg);

    Core::DecodeStatus Decode(const Core::ByteView& buf,
                              std::size_t& consumed,
                              Core::Frame& out);

    /// For this codec encoding is the identity: the frame payload is the frame.
    bool Encode(const Core::Frame& frame, std::vector<uint8_t>& out);

private:
    Core::FramingConfig _cfg;
    IFrameParserPtr _parser;        // LengthField only; Fixed uses _cfg.fixed
    std::vector<uint8_t> _frame;    // backs out.payload until the next Decode

    /// Fills type / typeName / header fields / repeat region from the LengthField spec.
    /// Returns false when the frame violates the spec (caller must drop the connection).
    bool ApplyFrameSpec(Core::Frame& out) const;

    /// Byte offset where the application payload starts (after the declared header).
    /// LengthField: headerLength, or lengthFieldOffset + encoded length width when 0.
    /// Fixed: 0 (the whole frame is the payload).
    std::size_t PayloadOffset() const;

    /// MSB-first bit slice of the frame (bit 0 = bit 7 of byte 0).
    static bool ExtractBits(const std::vector<uint8_t>& frame, int bitOffset,
                            int bitWidth, uint64_t& value);

    StreamFrameCodec(const StreamFrameCodec&);
    StreamFrameCodec& operator=(const StreamFrameCodec&);
};

}} // namespace MyProt::Transport

// src/Transport/src/StreamFrameCodec.cpp

#include "MyProt/Transport/StreamFrameCodec.hpp"

#include "MyProt/Transport/LengthFieldFrameParser.hpp"

namespace MyProt { namespace Transport {

StreamFrameCodec::StreamFrameCodec(const Core::FramingConfig& cfg)
    : _cfg(cfg) {
    if (_cfg.type == Core::FramingType::LengthField) {
        _parser.reset(new LengthFieldFrameParser(_cfg.lengthField));
    }
}

Core::DecodeStatus StreamFrameCodec::Decode(const Core::ByteView& buf,
                                            std::size_t& consumed,
                                            Core::Frame& out) {
    consumed = 0;

    if (_parser) {
        const Core::Expected<FrameParseResult> result = _parser->Parse(buf, 0);
        if (!result.has_value()) return Core::DecodeStatus::Malformed;
        if (result.value().needMoreData) return Core::DecodeStatus::NeedMore;
        if (result.value().consumedBytes == 0) return Core::DecodeStatus::Malformed;
        _frame = result.value().frame;
        consumed = result.value().consumedBytes;
    } else {
        const std::size_t need =
            static_cast<std::size_t>(_cfg.fixed.fixedLength);
        if (need == 0) return Core::DecodeStatus::Malformed;
        if (buf.size < need) return Core::DecodeStatus::NeedMore;
        _frame.assign(buf.data, buf.data + need);
        consumed = need;
    }

    out.Clear();
    if (!ApplyFrameSpec(out)) return Core::DecodeStatus::Malformed;
    const std::size_t offset = PayloadOffset();
    if (offset > _frame.size()) return Core::DecodeStatus::Malformed;
    out.payload = Core::ByteView(_frame.empty() ? 0 : &_frame[0] + offset,
                                 _frame.size() - offset);
    return Core::DecodeStatus::Complete;
}

std::size_t StreamFrameCodec::PayloadOffset() const {
    if (_cfg.type != Core::FramingType::LengthField) return 0;
    const Core::LengthFieldConfig& lf = _cfg.lengthField;
    if (lf.headerLength > 0) return static_cast<std::size_t>(lf.headerLength);

    // headerLength=0: the header is whatever precedes the length field plus the field itself.
    std::size_t width = static_cast<std::size_t>(lf.lengthFieldLength);
    if (lf.lengthEncoding == Core::LengthFieldEncoding::Varint) {
        width = 0;
        const std::size_t maxBytes = static_cast<std::size_t>(lf.varintMaxBytes);
        for (std::size_t i = 0; i < maxBytes; ++i) {
            const std::size_t at = static_cast<std::size_t>(lf.lengthFieldOffset) + i;
            if (at >= _frame.size()) break;
            ++width;
            if ((_frame[at] & 0x80) == 0) break;
        }
    }
    return static_cast<std::size_t>(lf.lengthFieldOffset) + width;
}

bool StreamFrameCodec::ApplyFrameSpec(Core::Frame& out) const {
    if (_cfg.type != Core::FramingType::LengthField) return true;
    const Core::LengthFieldConfig& lf = _cfg.lengthField;

    // 1) bit fields (ADR-0021 §2.2)
    for (size_t i = 0; i < lf.headerFields.size(); ++i) {
        const Core::HeaderFieldConfig& hf = lf.headerFields[i];
        uint64_t value = 0;
        if (!ExtractBits(_frame, hf.bitOffset, hf.bitWidth, value)) return false;
        out.SetUInt(hf.name, value);
    }

    // 2) dispatch: header value -> stable frame type (ADR-0021 §2.3)
    if (!lf.dispatch.field.empty()) {
        uint64_t raw = 0;
        if (!out.GetUInt(lf.dispatch.field, raw)) return false;
        bool matched = false;
        for (size_t i = 0; i < lf.dispatch.cases.size(); ++i) {
            const Core::DispatchCaseConfig& c = lf.dispatch.cases[i];
            if (c.value != raw) continue;
            out.type = c.type != 0 ? c.type : static_cast<uint32_t>(c.value);
            out.typeName = c.name;
            matched = true;
            break;
        }
        if (!matched) {
            if (lf.dispatch.rejectUnknown) return false;
            out.type = static_cast<uint32_t>(raw);
        }
    }

    // 3) repeated records (ADR-0021 §2.4)
    if (!lf.repeat.countField.empty()) {
        uint64_t count = 0;
        if (!out.GetUInt(lf.repeat.countField, count)) return false;
        if (count > static_cast<uint64_t>(lf.repeat.maxCount)) return false;
        const uint64_t region = count * static_cast<uint64_t>(lf.repeat.stride);
        const uint64_t start = static_cast<uint64_t>(lf.repeat.offset);
        if (start + region > static_cast<uint64_t>(_frame.size())) return false;
        if (region == 0) {
            out.SetBytes(lf.repeat.name, Core::ByteView());
        } else {
            out.SetBytes(lf.repeat.name,
                         Core::ByteView(&_frame[static_cast<size_t>(start)],
                                        static_cast<size_t>(region)));
        }
    }
    return true;
}

bool StreamFrameCodec::ExtractBits(const std::vector<uint8_t>& frame, int bitOffset,
                                   int bitWidth, uint64_t& value) {
    if (bitOffset < 0 || bitWidth < 1 || bitWidth > 32) return false;
    const size_t endBit = static_cast<size_t>(bitOffset) + static_cast<size_t>(bitWidth);
    if (endBit > frame.size() * 8) return false;

    uint64_t v = 0;
    for (int i = 0; i < bitWidth; ++i) {
        const size_t bit = static_cast<size_t>(bitOffset + i);
        const uint8_t byte = frame[bit / 8];
        const int shift = 7 - static_cast<int>(bit % 8);
        v = (v << 1) | static_cast<uint64_t>((byte >> shift) & 0x01);
    }
    value = v;
    return true;
}

bool StreamFrameCodec::Encode(const Core::Frame& frame, std::vector<uint8_t>& out) {
    out.clear();
    if (frame.payload.size > 0) {
        if (frame.payload.data == 0) return false;
        out.assign(frame.payload.data, frame.payload.data + frame.payload.size);
    }
    return true;
}

}} // namespace MyProt::Transport

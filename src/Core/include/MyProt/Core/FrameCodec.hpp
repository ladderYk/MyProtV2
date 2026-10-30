// src/Core/include/MyProt/Core/FrameCodec.hpp
// Generic frame layer: one message boundary + typed fields, independent of I/O.
//
// Why this exists (ADR-0020): a protocol stack is "byte stream -> frame layer -> session".
// The frame layer is stateless and describable by a byte grammar (header spec, length spec,
// field extraction); the session layer (handshake / keepalive / retransmit) is a state
// machine and stays hand-written. Splitting them keeps wire format in one place and lets a
// data-driven codec be plugged in later without touching session code.
//
// Contract:
//   - Decode() never blocks and never owns the input; it is pure framing + field extraction.
//   - Frame::payload is a view. It stays valid only until the next Decode() on the same
//     codec instance, or until the caller's buffer is modified. Bytes *fields* are copied
//     into Frame storage, so only the payload needs the lifetime caveat.
//   - A Frame is deliberately non-copyable: copying it would copy field storage but leave
//     the payload view pointing into someone else's buffer. Pass it by reference.
//   - C++11 only (no std::variant / std::optional; ADR-0010 §3).

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "MyProt/Core/ByteView.hpp"

namespace MyProt { namespace Core {

/// Result of one decode attempt. The three-way split matters for streaming: a truncated
/// frame is not an error, an unparsable one is.
enum class DecodeStatus {
    Complete,   // out / consumed are valid
    NeedMore,   // a partial frame is buffered; call again after more bytes arrive
    Malformed   // protocol violation; the caller should drop the connection
};

/// One decoded field value - tagged struct (no std::variant in C++11).
struct FrameField {
    enum Kind { UInt, Int, Bool, String, Bytes };

    Kind kind;
    uint64_t u;
    int64_t i;
    bool b;
    std::string s;
    std::vector<uint8_t> bytes;

    FrameField() : kind(UInt), u(0), i(0), b(false) {}
};

/// One decoded frame: a discriminator, raw flags, named fields and an opaque payload.
///
/// `payload` is the region the *next* layer consumes (MQTT PUBLISH body, HTTP body, or the
/// whole frame for a byte-stream-only codec). Everything the frame layer understands is
/// available through `fields` instead, so session code never touches byte layout.
class Frame {
public:
    Frame() : type(0), flags(0) {}
    ~Frame() {}

    uint32_t type;          // protocol discriminator (codec-defined; 0 = unset)
    uint32_t flags;         // raw header flags (codec-defined)
    std::string typeName;   // optional human-readable type (dispatch case name)
    ByteView payload;       // opaque application payload (view; see lifetime contract above)

    void Clear() {
        type = 0;
        flags = 0;
        typeName.clear();
        payload = ByteView();
        _fields.clear();
    }

    void SetUInt(const std::string& name, uint64_t v) {
        FrameField& f = Upsert(name);
        f.kind = FrameField::UInt;
        f.u = v;
    }

    void SetInt(const std::string& name, int64_t v) {
        FrameField& f = Upsert(name);
        f.kind = FrameField::Int;
        f.i = v;
    }

    void SetBool(const std::string& name, bool v) {
        FrameField& f = Upsert(name);
        f.kind = FrameField::Bool;
        f.b = v;
    }

    void SetString(const std::string& name, const std::string& v) {
        FrameField& f = Upsert(name);
        f.kind = FrameField::String;
        f.s = v;
    }

    void SetBytes(const std::string& name, const ByteView& v) {
        FrameField& f = Upsert(name);
        f.kind = FrameField::Bytes;
        if (v.size > 0 && v.data != 0) {
            f.bytes.assign(v.data, v.data + v.size);
        }
    }

    bool Has(const std::string& name) const { return Find(name) != 0; }

    bool GetUInt(const std::string& name, uint64_t& out) const {
        const FrameField* f = Find(name);
        if (!f || f->kind != FrameField::UInt) return false;
        out = f->u;
        return true;
    }

    bool GetInt(const std::string& name, int64_t& out) const {
        const FrameField* f = Find(name);
        if (!f || f->kind != FrameField::Int) return false;
        out = f->i;
        return true;
    }

    bool GetBool(const std::string& name, bool& out) const {
        const FrameField* f = Find(name);
        if (!f || f->kind != FrameField::Bool) return false;
        out = f->b;
        return true;
    }

    bool GetString(const std::string& name, std::string& out) const {
        const FrameField* f = Find(name);
        if (!f || f->kind != FrameField::String) return false;
        out = f->s;
        return true;
    }

    /// Returns a view into the Frame's own storage; valid while the Frame lives.
    bool GetBytes(const std::string& name, ByteView& out) const {
        const FrameField* f = Find(name);
        if (!f || f->kind != FrameField::Bytes) return false;
        out = ByteView(f->bytes.empty() ? 0 : &f->bytes[0], f->bytes.size());
        return true;
    }

private:
    const FrameField* Find(const std::string& name) const {
        for (std::size_t i = 0; i < _fields.size(); ++i) {
            if (_fields[i].first == name) return &_fields[i].second;
        }
        return 0;
    }

    FrameField& Upsert(const std::string& name) {
        for (std::size_t i = 0; i < _fields.size(); ++i) {
            if (_fields[i].first == name) {
                FrameField& f = _fields[i].second;
                f = FrameField();          // reset stale value of another kind
                return f;
            }
        }
        _fields.push_back(std::make_pair(name, FrameField()));
        return _fields.back().second;
    }

    std::vector<std::pair<std::string, FrameField> > _fields;

    Frame(const Frame&);
    Frame& operator=(const Frame&);
};

/// Frame codec: byte stream <-> Frame. Implementations own no socket and no thread.
class IFrameCodec {
public:
    virtual ~IFrameCodec() {}

    /// Attempt to decode one frame from the head of `buf`.
    /// @param consumed on Complete, the number of bytes this frame occupied (>= 1)
    virtual DecodeStatus Decode(const ByteView& buf,
                                std::size_t& consumed,
                                Frame& out) = 0;

    /// Encode a Frame back into wire bytes; false = the frame is not expressible.
    virtual bool Encode(const Frame& frame, std::vector<uint8_t>& out) = 0;
};

}} // namespace MyProt::Core

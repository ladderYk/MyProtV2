// src/Gateway/include/MyProt/Gateway/TagReader.hpp
// Tag reader - the sole pipeline holder in the Gateway layer (modules/05_Gateway.md §5.3)
// Single-read / batch-read / write share the same RequestBuilder -> IChannel::SendReceive ->
// ResponseParser assembly (the Engine module provides stateless primitives).

#pragma once
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"
#include "MyProt/Engine/ResponseParser.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Gateway/MergedRequest.hpp"

namespace MyProt { namespace Gateway {

/// Write-then-read-back verification parameters - parsed and assembled by the caller (the App assembly layer):
///   readOp/readVars  the operation template and variable table for building the read-back request (incl. address/length semantics)
///   expectedBytes    the expected data bytes (scalars encoded per finalType; variable-length = the written bytes)
///   tagLabel         diagnostic context (Error.context)
/// The data-region start is readOp.responseParser.dataStartIndex (<=0 is treated as a missing config).
struct WriteBackCheck {
    Core::OperationConfig readOp;
    std::unordered_map<std::string, std::uint32_t> readVars;
    std::vector<std::uint8_t> expectedBytes;
    std::string tagLabel;
        // Used to sync autoCompute rules for the read-back Build (empty = no-op).
    //   RuntimeGlue::BuildWriteBackCheck passes a snapshot of the protocol autoCompute section (via MergeOpAutoComputeJson).
        //   This field already merges the source=auto overrides from the operation inputs (BuildWriteBackCheck
    //   calls MergeOpAutoComputeJson after loading readOp, so it is no longer just a protocol snapshot).
    std::string autoComputeJson;
};

/// Protocol-level + op-level autoCompute merge (exposed to the App-layer RuntimeGlue).
///   The protocol level is rebuilt by RequestBuilder::CollectAutoComputeJson (extracted from inputs at runtime,
///   excluding outputs derived outputs - those are special-cased and injected at the WriteBytes stage).
///   The op-level override accumulates from op.inputs entries with source=auto (field-level override of same-named variables).
///   A protocol-level same-named key is overridden by the op-level one (later key overrides earlier key).
std::string MergeOpAutoComputeJson(const Core::ProtocolConfig& protocol,
                                   const Core::OperationConfig& op);

/// Tag reader - single read / batch read / single write
class TagReader {
public:
    using BatchHandler = std::function<void(std::vector<Core::TagValue>)>;
    using WriteHandler = std::function<void(Core::VoidExpected)>;

    TagReader();

    /// Batch-read a merged tag group
    /// Build one request -> send -> split the response from the shared tag table per merged.tagIndices.
    /// tagsArray/protocol are both shared snapshots - the async callback is kept alive via shared_ptr, zero-copy.
    void ReadBatch(const MergedRequest& merged,
                   std::shared_ptr<const std::vector<Core::TagDefinition>> tagsArray,
                   std::shared_ptr<const Core::ProtocolConfig> protocol,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   BatchHandler handler);

        /// Single-register write (/write): build the request per the protocol write-operation template -> send ->
    /// verify the echo (validCondition), without parsing the data region.
    /// Write-operation name convention "WriteSingleRegister"; variable table = tag.variables +
        /// StartAddress + {valueVariable}=value (the variable name is given by the caller;
    /// write tags use tag.writeVariable, legacy fixed "WriteValue").
        /// backCheck non-null: right after the write-echo check passes, build a read request per
    /// backCheck->readOp/readVars, using readOp.responseParser.dataStartIndex as the data-region start,
    /// comparing expectedBytes byte by byte; on mismatch, call back ReadBackMismatch.
    /// A null pointer = do not read back.
    // Note: the original P1 D busStrand parameter was withdrawn on 2026-08-29, see ADR-0011 §3.2
    void WriteOnce(const Core::TagDefinition& tag,
                   std::shared_ptr<const Core::ProtocolConfig> protocol,
                   const std::string& writeOperation,
                   std::uint32_t value,
                   const std::string& valueVariable,
                   const std::shared_ptr<const WriteBackCheck>& backCheck,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   WriteHandler handler);

    /// Variable-length byte write (P1 A, ADR-0007 §3 written-off item /write bytes):
    /// reuses the same pipeline as WriteOnce; additionally accepts a hex-string table injected into
    /// {Name:raw} placeholders (e.g. Modbus FC16 multi-register write, S7 ANY pointer).
    /// The write-operation name is given by the caller (e.g. "WriteMultipleRegisters");
    /// variable table = tag.variables + StartAddress + the contents of variableBytesHex.
        /// backCheck semantics are the same as WriteOnce (the read-back does not reuse the write-op template -
        /// the caller explicitly supplies the read op + expected bytes; reusing the write template would emit a wrong request).
    void WriteBytes(const Core::TagDefinition& tag,
                    std::shared_ptr<const Core::ProtocolConfig> protocol,
                    const std::string& writeOperation,
                    const std::unordered_map<std::string, std::string>& variableBytesHex,
                    const std::shared_ptr<const WriteBackCheck>& backCheck,
                    Transport::IChannel& channel,
                    int requestTimeoutMs,
                    WriteHandler handler);

private:
        /// Unified write-then-read-back executor: build the check->readOp read request -> send ->
    /// using readOp.responseParser.dataStartIndex as the data-region start, compare check->expectedBytes byte by byte.
    /// The terminal state calls back done exactly once.
    void RunReadBack(Transport::IChannel& channel,
                     const std::shared_ptr<const Core::FramingConfig>& framing,
                     int requestTimeoutMs,
                     const std::shared_ptr<const WriteBackCheck>& check,
                     const std::unordered_map<std::string, std::string>& varAliasMap,
                     const WriteHandler& done);
    /// P1 C (ADR-0011): per-device write mutex bit.
    /// On the same device, writes are mutually exclusive with writes; reads do not participate. Grab the slot on entry;
    /// a single callback path (success/failure/timeout terminal states) releases it. Independent of the io thread model,
    /// still effective if switched to multiple io_context run threads in the future.
    std::unordered_map<std::string, std::atomic<bool>> _writingInFlight;

    Engine::RequestBuilder _requestBuilder;
    Engine::ResponseParser _responseParser;
    Engine::AutoComputeProvider _autoProvider;
};

}} // namespace MyProt::Gateway

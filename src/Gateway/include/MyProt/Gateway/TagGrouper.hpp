// src/Gateway/include/MyProt/Gateway/TagGrouper.hpp
// Tag grouper - groups by (deviceId, operation, scanRate) + coalesces nearby addresses (modules/05_Gateway.md §5.4)

#pragma once
#include <vector>
#include <string>
#include "MyProt/Core/Config.hpp"
#include "MyProt/Gateway/MergedRequest.hpp"

namespace MyProt { namespace Gateway {

/// Tag group - the set of tags with the same (deviceId, operation, scanRate)
struct TagGroup {
    std::string deviceId;
    std::string operationName;
    int scanRateMs;
    std::vector<size_t> tagIndices;   // indices into the original tags array
};

/// Tag grouper - groups tags by device+operation+scan-period, then coalesces them into batch requests by address proximity
class TagGrouper {
public:
    /// Group by (deviceId, operation, scanRate)
    std::vector<TagGroup> GroupByScanRate(
        const std::vector<Core::TagDefinition>& tags);

    /// Coalesce adjacent-address tags within a group into a list of MergedRequests
    /// @param group the grouping result
    /// @param tags the original tag array (used to read the addresses in variables)
    /// @param maxSpan the maximum coalesced address span (bytes; split if exceeded).
    ///        The actual caller (PollingEngine) passes the protocol-level ProtocolConfig::maxSpanBytes;
    ///        the default here is only a fallback for the direct-construction path.
    std::vector<MergedRequest> CoalesceAdjacent(
        const TagGroup& group,
        const std::vector<Core::TagDefinition>& tags,
        int maxSpan = Core::kDefaultMaxSpanBytes);

    /// Convenience interface: complete grouping + coalescing in one step
    /// @param tags the tag definitions to group
    /// @param maxSpan the maximum address span (bytes; do not coalesce if exceeded)
    /// @return the coalesced request list
    std::vector<MergedRequest> Group(
        const std::vector<Core::TagDefinition>& tags,
        int maxSpan = Core::kDefaultMaxSpanBytes);

    /// Extract the start byte address from tag.variables.
        ///   It is the cross-protocol byte unit "StartByteAddress" (protocol-family address names such as the Modbus register number
        ///   are derived by the protocol JSON outputs' derivedLength); there is no addressVariable config.
    ///   Single point of definition - pipeline components such as TagReader share it, no longer each copying it.
    static uint32_t GetStartAddress(const Core::TagDefinition& tag);

        /// Extract the byte span from tag.variables["ByteCount"].
    ///   Replaces the old tag.registerCount x 2 (Modbus-protocol-family hardcoding);
    ///   the engine has zero protocol knowledge, the byte count is fully expressed by the protocol JSON derivedLength.
    ///   Not declared -> fall back to 2 (the UInt16 minimum span, compatible with the early finalType default).
    static uint32_t GetByteCount(const Core::TagDefinition& tag);

private:
};

}} // namespace MyProt::Gateway

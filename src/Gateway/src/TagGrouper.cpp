// src/Gateway/src/TagGrouper.cpp — 标签分组器实现
// 两步流水线: ①GroupByScanRate 按 (device, op, scanRate) 分组
//              ②CoalesceAdjacent 按地址邻近性合并为批量请求
#include "MyProt/Gateway/TagGrouper.hpp"
#include <algorithm>
#include <map>
#include <limits>

namespace MyProt { namespace Gateway {

// ── 提取起始地址 ──

uint32_t TagGrouper::GetStartAddress(const Core::TagDefinition& tag) {
    // 读跨协议字节单位 "StartByteAddress" (协议 JSON derivedLength 派生前已是字节).
    //   Modbus 侧标签声明字节地址 "StartByteAddress": N×2 (协议族寄存器号由 outputs.derivedLength 派生),
    //   协议 JSON 通过 outputs.StartAddress = StartByteAddress/2 派生寄存器号给 FC03 命令字用.
    auto it = tag.variables.find(Core::StartByteAddressVariableName());
    return (it != tag.variables.end()) ? it->second : 0;
}

// ── 提取字节跨度 (不按 registerCount×2 推算, 而是按协议声明) ──
//   由协议 JSON 通过 outputs.ByteCount (derivedLength) 派生;
//   Modbus: {RegisterCount} * 2; S7: 直接字节数.
//   引擎零协议知识, 仅按 "ByteCount" 约定键查表; tag 没声明 → 回退 2 字节
//   (UInt16 默认类型最小跨度, 失败路径由 ResponseParser 进一步捕获).
uint32_t TagGrouper::GetByteCount(const Core::TagDefinition& tag) {
    auto it = tag.variables.find(Core::ByteCountVariableName());
    return (it != tag.variables.end()) ? it->second : 2;
}

// ── 按 (deviceId, operation, scanRate) 分组 ──

std::vector<TagGroup> TagGrouper::GroupByScanRate(
    const std::vector<Core::TagDefinition>& tags) {

    // 使用有序 map 保证确定性输出顺序
    std::map<std::tuple<std::string, std::string, int>, TagGroup> groups;

    for (size_t i = 0; i < tags.size(); ++i) {
        const auto& tag = tags[i];
        auto key = std::make_tuple(tag.deviceId, tag.operation, tag.scanRateMs);
        auto& group = groups[key];
        if (group.tagIndices.empty()) {
            group.deviceId = tag.deviceId;
            group.operationName = tag.operation;
            group.scanRateMs = tag.scanRateMs;
        }
        group.tagIndices.push_back(i);
    }

    std::vector<TagGroup> result;
    result.reserve(groups.size());
    for (auto& pair : groups) {
        result.push_back(std::move(pair.second));
    }
    return result;
}

// ── 地址邻近合并 ──

std::vector<MergedRequest> TagGrouper::CoalesceAdjacent(
    const TagGroup& group,
    const std::vector<Core::TagDefinition>& tags,
    int maxSpan) {

    if (group.tagIndices.empty()) {
        return {};
    }

    // 按起始字节地址排序 (字节语义; 协议族单位由协议 JSON derivedLength 解释)
    std::vector<size_t> sorted = group.tagIndices;
    std::sort(sorted.begin(), sorted.end(),
        [&tags](size_t a, size_t b) {
            return GetStartAddress(tags[a]) < GetStartAddress(tags[b]);
        });

    std::vector<MergedRequest> result;

    // 贪心合并: 当前批次的 [startByte, endByte]
    size_t batchStart = 0;  // sorted 中的起始索引
    uint32_t startAddr = GetStartAddress(tags[sorted[0]]);
    // 跨度 = tag.variables["ByteCount"] (协议 JSON derivedLength 派生后填入)
    uint32_t endAddr = startAddr + GetByteCount(tags[sorted[0]]);

    for (size_t i = 1; i <= sorted.size(); ++i) {
        bool flush = (i == sorted.size());
        if (!flush) {
            uint32_t nextAddr = GetStartAddress(tags[sorted[i]]);
            uint32_t nextEnd = nextAddr + GetByteCount(tags[sorted[i]]);

            // 单地址读 (coalesce=false) 不参与合并 → 强制分断为独立请求
            flush = !tags[sorted[i]].coalesce || !tags[sorted[batchStart]].coalesce;
            if (!flush) {
                // 检查能否合并: 新跨度不超过 maxSpan (字节)
                flush = (nextEnd - startAddr) > static_cast<uint32_t>(maxSpan);
                if (!flush) {
                    // 扩展当前批次范围
                    if (nextEnd > endAddr) {
                        endAddr = nextEnd;
                    }
                }
            }
        }

        if (flush) {
            MergedRequest merged;
            merged.deviceId = group.deviceId;
            merged.operation = group.operationName;
            merged.startByteAddress = startAddr;
            merged.byteCount = endAddr - startAddr;

            for (size_t j = batchStart; j < i; ++j) {
                merged.tagIndices.push_back(sorted[j]);
            }

            result.push_back(std::move(merged));

            // 开始新批次
            if (i < sorted.size()) {
                batchStart = i;
                startAddr = GetStartAddress(tags[sorted[i]]);
                endAddr = startAddr + GetByteCount(tags[sorted[i]]);
            }
        }
    }

    return result;
}

// ── 便捷接口: 分组 + 合并一步完成 ──

std::vector<MergedRequest> TagGrouper::Group(
    const std::vector<Core::TagDefinition>& tags,
    int maxSpan) {

    auto groups = GroupByScanRate(tags);

    std::vector<MergedRequest> result;
    for (const auto& group : groups) {
        auto merged = CoalesceAdjacent(group, tags, maxSpan);
        result.insert(result.end(), merged.begin(), merged.end());
    }
    return result;
}

}} // namespace MyProt::Gateway

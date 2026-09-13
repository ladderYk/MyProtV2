// src/Gateway/include/MyProt/Gateway/TagGrouper.hpp
// 标签分组器 — 按 (deviceId, operation, scanRate) 分组 + 地址邻近合并 (modules/05_Gateway.md §5.4)

#pragma once
#include <vector>
#include <string>
#include "MyProt/Core/Config.hpp"
#include "MyProt/Gateway/MergedRequest.hpp"

namespace MyProt { namespace Gateway {

/// 标签分组 — 同一 (deviceId, operation, scanRate) 的标签集合
struct TagGroup {
    std::string deviceId;
    std::string operationName;
    int scanRateMs;
    std::vector<size_t> tagIndices;   // 原始 tags 数组中的索引
};

/// 标签分组器 — 将标签按设备+操作+扫描周期分组, 再按地址邻近性合并为批量请求
class TagGrouper {
public:
    /// 按 (deviceId, operation, scanRate) 分组
    std::vector<TagGroup> GroupByScanRate(
        const std::vector<Core::TagDefinition>& tags);

    /// 将分组内相邻地址标签合并为 MergedRequest 列表
    /// @param group 分组结果
    /// @param tags 原始标签数组 (用于读取 variables 中的地址)
    /// @param maxSpan 最大合并地址跨度 (字节; 超过则拆分).
    ///        实际调用方 (PollingEngine) 传协议级 ProtocolConfig::maxSpanBytes;
    ///        此处缺省仅为直接构造路径兜底.
    std::vector<MergedRequest> CoalesceAdjacent(
        const TagGroup& group,
        const std::vector<Core::TagDefinition>& tags,
        int maxSpan = Core::kDefaultMaxSpanBytes);

    /// 便捷接口: 一步完成分组 + 合并
    /// @param tags 待分组的标签定义
    /// @param maxSpan 最大地址跨度 (字节; 超过则不合并)
    /// @return 合并后的请求列表
    std::vector<MergedRequest> Group(
        const std::vector<Core::TagDefinition>& tags,
        int maxSpan = Core::kDefaultMaxSpanBytes);

    /// 从 tag.variables 中提取起始字节地址.
    ///   v1.25 起为跨协议字节单位 "StartByteAddress" (协议族地址名如 Modbus 的寄存器号
    ///   由协议 JSON outputs 的 derivedLength 派生); v1.19 已移除 addressVariable 配置.
    ///   唯一定义处 — TagReader 等管线组件共用, 不再各自复制
    static uint32_t GetStartAddress(const Core::TagDefinition& tag);

    /// v1.25: 从 tag.variables["ByteCount"] 提取字节跨度.
    ///   替代旧的 tag.registerCount × 2 (Modbus 协议族硬编码);
    ///   引擎零协议知识, 字节数完全由协议 JSON derivedLength 表达.
    ///   没声明 → 回退 2 (UInt16 最小跨度, 与早期 finalType 默认值兼容).
    static uint32_t GetByteCount(const Core::TagDefinition& tag);

private:
};

}} // namespace MyProt::Gateway

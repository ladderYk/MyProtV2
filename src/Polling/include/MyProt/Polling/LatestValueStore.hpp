// src/Polling/include/MyProt/Polling/LatestValueStore.hpp
// 最新值快照存储 — 保存每个标签最近一次采集结果, 供 /api/data/latest 查询
// 线程安全: PollingEngine io 线程写入 (Update), WebApi 线程读取 (Snapshot)

#pragma once
#include <map>
#include <string>
#include <vector>
#include <mutex>

#include "MyProt/Core/Value.hpp"

namespace MyProt { namespace Polling {

class LatestValueStore {
public:
    /// 批量更新最新值 (PollingEngine 回调线程调用; 同名标签后写覆盖先写)
    void Update(const std::vector<Core::TagValue>& values);

    /// 全量快照 (按 tagName 排序); deviceFilter 非空时按 deviceId 前缀过滤
    std::vector<Core::TagValue> Snapshot(
        const std::string& deviceFilter = std::string()) const;

    /// 单标签查询 (v1.1 补): 设备离线期间用上次值替代 Bad 输出
    /// @return true 且 out 填充 — 存在; false — 标签从未有值
    bool Lookup(const std::string& tagName, Core::TagValue& out) const;

    /// 当前标签数
    size_t Count() const;

    /// 清空快照 (配置热重载后调用, 丢弃旧标签集的陈旧数据)
    void Clear();

private:
    mutable std::mutex _mutex;
    std::map<std::string, Core::TagValue> _latest;   // tagName → 最近值
};

}} // namespace MyProt::Polling

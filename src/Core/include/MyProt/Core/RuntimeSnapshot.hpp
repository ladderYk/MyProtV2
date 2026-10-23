// src/Core/include/MyProt/Core/RuntimeSnapshot.hpp
// 运行时配置快照 — 加载/热重载的唯一产物。
//
// 历史背景: AppContext 曾同时持有 protos / protoStore，且 ApplyRuntimeSync 内
// 还会再复制一份 LoadedConfig，同一代配置在内存中存在 3 份副本，各自被不同
// 线程读取 (写路径 / 仿真器 / 轮询装配)，出现副本漂移与悬垂引用的风险。
// 现在统一为: 一次加载 → 一份不可变 RuntimeSnapshot → 所有消费者共享。
//
// 使用约定:
//   - 快照一旦发布即只读 (取 const 指针)，热重载整代替换而非原地改写；
//   - 读侧通过 RuntimeSnapshotHolder::Get() 取 shared_ptr，天然保活；
//   - 需要长期持有的组件 (如 SimulationServer) 保存 shared_ptr (或别名
//     shared_ptr 指向快照内的单个元素)，不得保存裸引用。
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Optional.hpp"
#include "MyProt/Core/ServerConfig.hpp"

namespace MyProt { namespace Core {

/// 一代运行时配置 (协议 + 设备 + 标签 + 服务端配置)，发布后不可变。
struct RuntimeSnapshot {
    std::vector<ProtocolConfig> protocols;
    std::vector<DeviceConfig> devices;
    std::vector<TagDefinition> tags;
    Optional<ResilienceConfig> resilience;   ///< 全局韧性策略 (缺省 = 全默认)
    ServerConfig server;                     ///< server.json (仿真段等)

    const ProtocolConfig* FindProtocol(const std::string& name) const {
        for (std::size_t i = 0; i < protocols.size(); ++i) {
            if (protocols[i].protocolName == name) return &protocols[i];
        }
        return 0;
    }

    const DeviceConfig* FindDevice(const std::string& id) const {
        for (std::size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].id == id) return &devices[i];
        }
        return 0;
    }
};

typedef std::shared_ptr<const RuntimeSnapshot> RuntimeSnapshotPtr;

/// 线程安全快照持有者: 读侧取 shared_ptr，写侧整代替换。
/// 读操作只持锁拷贝 shared_ptr，不覆盖快照内的读取代价。
class RuntimeSnapshotHolder {
public:
    RuntimeSnapshotHolder() {}

    RuntimeSnapshotPtr Get() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current;
    }

    void Set(const RuntimeSnapshotPtr& next) {
        std::lock_guard<std::mutex> lock(_mutex);
        _current = next;
    }

private:
    RuntimeSnapshotHolder(const RuntimeSnapshotHolder&);            // 不可拷贝
    RuntimeSnapshotHolder& operator=(const RuntimeSnapshotHolder&);

    mutable std::mutex _mutex;
    RuntimeSnapshotPtr _current;
};

}} // namespace MyProt::Core

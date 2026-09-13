// src/Service/include/MyProt/Service/DeviceLifecycle.hpp
// 设备生命周期状态机 (KI-04)
//
// 设计要点 — 与熔断器正交:
//   * 熔断器 (SessionContext::CircuitState) 是流量层瞬态许可, 影响
//     "请求是否放行", 可恢复 (HalfOpen 探测闭合)。
//   * 生命周期 (DeviceLifecycleState) 是设备级持久维度, 表示
//     "设备当前是否可用 / 正在做什么", 由连接/握手/重连/热重载触发。
//   * 两者并存: Degraded 设备熔断可仍 Closed, Open 熔断下设备仍可 Connected
//     (只是没流量过)。Cancel 替代关系不存在。
//
// 状态机迁移表 (modules/04_Service.md §3, ADR-0004 §6):
//
//                  ┌────────── Disabled (永久禁用: enabled=false / 不可恢复构建错)
//                  ▲
//   New ──(GOC)──► Connecting ──(握手+连接成功)──► Connected
//                    │                                │
//                    │                                │(连接级故障/通道死)
//                    │                                ▼
//                    └─(连接/握手失败)──► Degraded ◄──┘
//                                                  │
//                              (下一轮 GOC)───────┘
//                                                  │
//                            (重连成功)────────────► Connected
//
// 触发者表:
//   New → Connecting            ChannelManager::GetOrCreateChannel 入口
//   Connecting → Connected      PerformConnect OK + 握手序列完成
//   Connecting → Degraded       PerformConnect 失败 或 PerformHandshake 失败
//                                或 handshake 模板 BuildError
//   Connected → Degraded        PollingEngine 整设备段 Bad 且属连接级故障
//                                (网络/超时/通道死) — 协议级不迁移
//   Degraded → Connecting       下一轮 timer 触发 GOC
//   Degraded → Connected        GOC 成功完成
//   * → Disabled                device.enabled=false (ConfigValidator 守门)
//                                或协议/握手 BuildError
//   Disabled → Connecting       配置热重载恢复 enabled (ResetDevices 重建)
//
// 故障分类 (PollingEngine 侧):
//   连接级 (→ Degraded): ConnectFailure / Timeout / NetworkError /
//                        ChannelDead (读写返回 ConnectionLost 类)
//   协议级 (不动生命周期): InvalidResponse / ParseError /
//                          ProtocolNotFound / TagNotFound
//   业务级 (不动生命周期): Bad 数据值但已收到响应
//
// 指标:
//   gauge   myprot_device_lifecycle_state{device,state}   当前态 (0/1)
//   counter myprot_device_lifecycle_transitions_total{device,from,to}

#pragma once
#include <string>

namespace MyProt { namespace Service {

/// 设备生命周期状态 — 持久维度, 描述设备"是否能用 / 正在做什么"
enum class DeviceLifecycleState {
    New = 0,        // 已注册, 从未连接
    Connecting = 1, // 物理连接/握手中
    Connected = 2,  // 物理通道+握手均已建立, 业务可用
    Degraded = 3,   // 通道死了或正在重连, 未到永久禁用门槛
    Disabled = 4    // 永久禁用 (enabled=false 或不可恢复构建错误)
};

/// 状态名 (供指标 label / 日志使用)
inline const char* DeviceLifecycleName(DeviceLifecycleState s) {
    switch (s) {
        case DeviceLifecycleState::New:        return "New";
        case DeviceLifecycleState::Connecting: return "Connecting";
        case DeviceLifecycleState::Connected:  return "Connected";
        case DeviceLifecycleState::Degraded:   return "Degraded";
        case DeviceLifecycleState::Disabled:   return "Disabled";
        default:                               return "Unknown";
    }
}

/// gauge 值: 用单条 gauge + state 标签还是按状态各一条?
/// 此处采用每设备单 gauge: value = state 编号, label 仅 device。
inline int DeviceLifecycleGaugeValue(DeviceLifecycleState s) {
    return static_cast<int>(s);
}

}} // namespace MyProt::Service

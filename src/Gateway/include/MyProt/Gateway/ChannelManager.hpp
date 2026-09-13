// src/Gateway/include/MyProt/Gateway/ChannelManager.hpp
// 通道管理器 — 设备级通道生命周期管理 (modules/05_Gateway.md §5.2, ADR-0002)
// 回调式异步模型 (C++11, ADR-0010, 无协程)

#pragma once
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <functional>
#include <chrono>
#include <asio.hpp>

#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Service/SessionContext.hpp"
#include "MyProt/Gateway/ProtocolLookup.hpp"

namespace MyProt { namespace Gateway {

/// 连接结果 — 通道 (会话上下文改由 ChannelManager::GetSession(deviceId) 获取)
struct ConnectResult {
    std::shared_ptr<Transport::IChannel> channel;
    // 注: P1 D per-bus strand (ADR-0011 §3.2) 于 2026-08-29 撤回,
    // 详见 ADR-0011 §3.2 末尾"撤回说明"。单 io_context 部署下,串行化
    // 由 io_context 自身保证,不需要额外 strand 间接层。
};

/// 通道工厂 — 由 ProtocolGateway 注入, 创建具体通道实例 (如 TcpChannel)
using ChannelFactory = std::function<std::shared_ptr<Transport::IChannel>(
    asio::io_context& io, const std::string& channelId)>;

/// 通道管理器 — 按设备管理通道生命周期 (连接/断开/重连)
/// 共享总线: 同一物理端点 (host:port) 的多设备共享 IChannel 实例 (ADR-0002 §4)
class ChannelManager {
public:
    ChannelManager(asio::io_context& io,
                   ProtocolLookup lookup,
                   ChannelFactory factory);

    /// 注册设备配置 (GetOrCreateChannel 前须先注册)
    void RegisterDevice(const Core::DeviceConfig& device);

    /// 批量注册设备
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    /// 重置设备表并按新配置重新注册 (配置热重载:
    /// 清除已删除设备的残留 entry; 须在 ShutdownAll 之后调用)
    void ResetDevices(const std::vector<Core::DeviceConfig>& devices);

    /// 获取或创建通道 (首次自动 Connect; 异步回调)
    void GetOrCreateChannel(const std::string& deviceId,
                            std::function<void(Core::Expected<ConnectResult>)> handler);

    /// 关闭所有通道
    void ShutdownAll();

    /// 获取会话上下文 (供 TagReader 注入会话变量)
    std::shared_ptr<Service::SessionContext> GetSession(const std::string& deviceId);

private:
    struct ChannelEntry {
        std::shared_ptr<Transport::IChannel> channel;
        std::shared_ptr<Service::SessionContext> session;
        Core::DeviceConfig deviceConfig;
        bool connecting;        // 避免并发创建同一通道
        // ── 冷却期: 物理连接连续失败后, 短期内再次 GetOrCreateChannel
        //    直接返回 ConnectionClosed, 避免 N 个 PollGroup × 1s 周期
        //    在断线期间每轮并发 Connect 形成风暴。
        //    冷却期 2s (单次连接超时 1-3s, 略大于 1 次尝试耗时)。
        int failedStreak;       // 连续失败次数 (成功后清零)
        std::chrono::steady_clock::time_point lastFailedAt;  // 最近失败时刻

        ChannelEntry() : connecting(false), failedStreak(0) {}
    };

    asio::io_context& _io;
    ProtocolLookup _lookup;
    ChannelFactory _factory;

    std::unordered_map<std::string, ChannelEntry> _devices;   // key = deviceId
    std::unordered_map<std::string, std::shared_ptr<Transport::IChannel>> _endpointCache;  // key = endpoint
    // 注: _endpointStrands / _endpointRefs (P1 D) 于 2026-08-29 撤回,见 ADR-0011 §3.2
    // recursive_mutex: GetOrCreateChannel 持锁调用 PerformConnect,
    // 后者内部需再次加锁访问端点缓存 (同线程同步重入)
    mutable std::recursive_mutex _mutex;

    /// 计算物理端点标识 (ADR-0002 §4)
    static std::string ComputeEndpointKey(const Core::ConnectionConfig& conn,
                                          const Core::TransportConfig& transport);

    /// 建立 TCP/物理连接 (不含协议握手)
    void PerformConnect(ChannelEntry& entry,
                        const Core::ProtocolConfig& protocol,
                        std::function<void(Core::Expected<ConnectResult>)> handler);

    /// 在已连接通道上顺序执行 protocol.handshake 各步骤
    /// (发送请求模板 → 校验 validCondition → 提取 sessionVariable)
    void PerformHandshake(
        std::shared_ptr<Transport::IChannel> channel,
        std::shared_ptr<Service::SessionContext> session,
        std::shared_ptr<const Core::ProtocolConfig> protocol,
        const Core::DeviceConfig& device,
        size_t stepIndex,
        std::chrono::milliseconds defaultTimeout,
        std::function<void(Core::Expected<void>)> handler);
};

}} // namespace MyProt::Gateway

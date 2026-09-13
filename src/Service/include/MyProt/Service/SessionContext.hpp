// src/Service/include/MyProt/Service/SessionContext.hpp
// 会话上下文 — 管理设备会话状态与韧性策略 (modules/04_Service.md, ADR-0004)

#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退)
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Service/DeviceLifecycle.hpp"

namespace MyProt { namespace Service {

/// 断路器状态
enum class CircuitState { Closed, Open, HalfOpen };

/// 设备会话上下文 — 管理连接状态、握手会话变量、韧性策略、生命周期 (KI-04)
class SessionContext {
public:
    explicit SessionContext(const Core::DeviceConfig& config);

    /// 获取会话变量 (握手阶段提取)
    Core::Optional<std::string> GetSessionVar(const std::string& name) const;

    /// 设置会话变量
    void SetSessionVar(const std::string& name, const std::string& value);

    /// 记录一次读取失败, 可能触发熔断
    void RecordFailure();

    /// 记录一次读取成功
    void RecordSuccess();

    /// 检查断路器是否允许请求
    bool CanProceed() const;

    /// 获取当前断路器状态
    CircuitState GetCircuitState() const;

    /// 设置设备生命周期状态 (KI-04; 由 ChannelManager / PollingEngine 触发)
    /// 状态变化时记录 metric gauge + 转换 counter + 日志
    void SetLifecycle(DeviceLifecycleState s);

    /// 获取当前生命周期状态
    DeviceLifecycleState GetLifecycle() const;

private:
    Core::DeviceConfig _config;
    std::unordered_map<std::string, std::string> _sessionVars;
    mutable std::mutex _mutex;

    // 韧性状态
    Core::ResilienceConfig _resilience;
    // CanProceed() 需在 const 下惰性完成 Open→HalfOpen 转换, 故状态字段可变
    mutable CircuitState _circuitState;
    int _consecutiveFailures;
    std::chrono::steady_clock::time_point _openedAt;  // 熔断器打开时刻 (冷却计时起点)
    mutable int _halfOpenProbes;

    // 设备生命周期 (KI-04) — 与熔断器正交: 描述"设备是否能用",
    // 不影响"请求是否放行"。详见 DeviceLifecycle.hpp
    DeviceLifecycleState _lifecycle;
};

}} // namespace MyProt::Service

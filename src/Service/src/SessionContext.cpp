// src/Service/src/SessionContext.cpp — 会话上下文与熔断器状态机 (ADR-0004)
#include "MyProt/Service/SessionContext.hpp"
#include "MyProt/Core/Log.hpp"
#include "MyProt/Core/Metrics.hpp"

namespace MyProt { namespace Service {

namespace {
// 设备级覆盖缺失时使用的默认韧性策略 (与 ResilienceConfig 默认构造一致)
Core::ResilienceConfig EffectiveResilience(const Core::DeviceConfig& config) {
    if (config.resilience.has_value()) return config.resilience.value();
    return Core::ResilienceConfig();
}

// 熔断器状态 → gauge 值 (0=Closed 1=HalfOpen 2=Open)
double CircuitGaugeValue(CircuitState s) {
    return s == CircuitState::HalfOpen ? 1.0
         : s == CircuitState::Open     ? 2.0 : 0.0;
}
} // namespace

SessionContext::SessionContext(const Core::DeviceConfig& config)
    : _config(config)
    , _resilience(EffectiveResilience(config))
    , _circuitState(CircuitState::Closed)
    , _consecutiveFailures(0)
    , _halfOpenProbes(0)
    , _openedAt()
    , _lifecycle(DeviceLifecycleState::New) {
    // 熔断状态 gauge 从设备创建起即有序列 (0=Closed), 避免监控面板缺口
    Core::metrics::GaugeSet(Core::metrics::kCircuitState,
                            Core::metrics::Device(_config.id), 0.0);
    // 设备生命周期 gauge 从设备创建起即有序列 (0=New), 监控面板无缺口
    // (KI-04: 真正不可用者 (协议/握手 BuildError 等) 由 ChannelManager 迁入 Disabled)
    Core::metrics::GaugeSet(Core::metrics::kDeviceLifecycleState,
                            Core::metrics::Device(_config.id), 0.0);
}

Core::Optional<std::string> SessionContext::GetSessionVar(const std::string& name) const {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessionVars.find(name);
    if (it != _sessionVars.end()) {
        return Core::Optional<std::string>(it->second);
    }
    return Core::Optional<std::string>();
}

void SessionContext::SetSessionVar(const std::string& name, const std::string& value) {
    std::lock_guard<std::mutex> lock(_mutex);
    _sessionVars[name] = value;
}

void SessionContext::RecordFailure() {
    std::lock_guard<std::mutex> lock(_mutex);
    _consecutiveFailures++;

    // HalfOpen 期间任一探测失败 → 立即重新 Open, 重置冷却
    if (_circuitState == CircuitState::HalfOpen) {
        _circuitState = CircuitState::Open;
        _openedAt = std::chrono::steady_clock::now();
        _halfOpenProbes = 0;
        LOG_WARN("Circuit", "设备 %s HalfOpen 探测失败, 重新熔断 (冷却 %dms)",
                 _config.id.c_str(), _resilience.cooldownMs);
        Core::metrics::CounterInc(Core::metrics::kCircuitOpensTotal,
                                  Core::metrics::Device(_config.id));
        Core::metrics::GaugeSet(Core::metrics::kCircuitState,
                                Core::metrics::Device(_config.id),
                                CircuitGaugeValue(_circuitState));
        return;
    }

    // Closed: 连续失败达到阈值 → Open
    if (_circuitState == CircuitState::Closed &&
        _consecutiveFailures >= _resilience.failureThreshold) {
        _circuitState = CircuitState::Open;
        _openedAt = std::chrono::steady_clock::now();
        _halfOpenProbes = 0;
        LOG_WARN("Circuit",
                 "设备 %s 连续失败 %d 次达到阈值, 熔断开启 (冷却 %dms)",
                 _config.id.c_str(), _consecutiveFailures,
                 _resilience.cooldownMs);
        Core::metrics::CounterInc(Core::metrics::kCircuitOpensTotal,
                                  Core::metrics::Device(_config.id));
        Core::metrics::GaugeSet(Core::metrics::kCircuitState,
                                Core::metrics::Device(_config.id),
                                CircuitGaugeValue(_circuitState));
    }
}

void SessionContext::RecordSuccess() {
    std::lock_guard<std::mutex> lock(_mutex);
    const bool recovered = (_circuitState != CircuitState::Closed);
    _consecutiveFailures = 0;
    // HalfOpen 探测成功 → 恢复 Closed (探测期间可能多笔成功, 直接关闭)
    _circuitState = CircuitState::Closed;
    _halfOpenProbes = 0;
    if (recovered) {
        LOG_INFO("Circuit", "设备 %s 熔断恢复 Closed", _config.id.c_str());
        Core::metrics::GaugeSet(Core::metrics::kCircuitState,
                                Core::metrics::Device(_config.id),
                                CircuitGaugeValue(_circuitState));
    }
}

bool SessionContext::CanProceed() const {
    std::lock_guard<std::mutex> lock(_mutex);
    if (_circuitState == CircuitState::Closed) return true;

    if (_circuitState == CircuitState::Open) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - _openedAt).count();
        if (elapsed >= _resilience.cooldownMs) {
            // 冷却结束 → 转 HalfOpen, 放行一笔探测
            _circuitState = CircuitState::HalfOpen;
            _halfOpenProbes = _resilience.halfOpenProbes > 0
                                  ? _resilience.halfOpenProbes
                                  : 1;
            LOG_INFO("Circuit", "设备 %s 冷却结束, 转 HalfOpen 探测",
                     _config.id.c_str());
            Core::metrics::GaugeSet(Core::metrics::kCircuitState,
                                    Core::metrics::Device(_config.id),
                                    CircuitGaugeValue(_circuitState));
            return true;
        }
        return false;  // 冷却中 → 快速失败
    }

    // HalfOpen: 尚有探测配额则放行
    if (_halfOpenProbes > 0) {
        --_halfOpenProbes;
        return true;
    }
    return false;
}

CircuitState SessionContext::GetCircuitState() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _circuitState;
}

void SessionContext::SetLifecycle(DeviceLifecycleState s) {
    DeviceLifecycleState from;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_lifecycle == s) return;  // 幂等: 无变化不计转换
        from = _lifecycle;
        _lifecycle = s;
    }
    // 锁外做 IO (metric / log), 避免持锁开销
    const std::string deviceId = _config.id;
    Core::metrics::GaugeSet(Core::metrics::kDeviceLifecycleState,
                            Core::metrics::Device(deviceId),
                            static_cast<double>(DeviceLifecycleGaugeValue(s)));
    // 转换 counter: 三标签 (device, from, to), 用 MakeKey 路径直接走 Registry
    Core::MetricsRegistry::Labels lbl;
    lbl.push_back(std::make_pair(std::string("device"), deviceId));
    lbl.push_back(std::make_pair(std::string("from"), DeviceLifecycleName(from)));
    lbl.push_back(std::make_pair(std::string("to"), DeviceLifecycleName(s)));
    Core::MetricsRegistry::Instance().CounterAdd(
        Core::metrics::kDeviceLifecycleTransitionsTotal, lbl, 1.0);
    // INFO 级日志 (转换非高频, 一设备一状态变化一次)
    LOG_INFO("Lifecycle", "设备 %s 生命周期 %s → %s",
             deviceId.c_str(),
             DeviceLifecycleName(from), DeviceLifecycleName(s));
}

DeviceLifecycleState SessionContext::GetLifecycle() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _lifecycle;
}

}} // namespace MyProt::Service

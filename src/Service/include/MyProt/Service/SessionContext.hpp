// src/Service/include/MyProt/Service/SessionContext.hpp
// Session context - manages device session state and resilience policy (modules/04_Service.md, ADR-0004)

#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include "MyProt/Core/Optional.hpp"   // Core::Optional<T> (C++11-compatible, replaces std::optional, rolled back 2026-08-29)
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Service/DeviceLifecycle.hpp"

namespace MyProt { namespace Service {

/// Circuit-breaker state
enum class CircuitState { Closed, Open, HalfOpen };

/// Device session context - manages connection state, handshake session variables, resilience policy, lifecycle (KI-04)
class SessionContext {
public:
    explicit SessionContext(const Core::DeviceConfig& config);

    /// Get a session variable (extracted during the handshake stage)
    Core::Optional<std::string> GetSessionVar(const std::string& name) const;

    /// Set a session variable
    void SetSessionVar(const std::string& name, const std::string& value);

    /// Record one read failure, which may trip the circuit breaker
    void RecordFailure();

    /// Record one read success
    void RecordSuccess();

    /// Check whether the circuit breaker allows a request
    bool CanProceed() const;

    /// Get the current circuit-breaker state
    CircuitState GetCircuitState() const;

    /// Set the device lifecycle state (KI-04; triggered by ChannelManager / PollingEngine)
    /// On a state change, record the metric gauge + transition counter + log
    void SetLifecycle(DeviceLifecycleState s);

    /// Get the current lifecycle state
    DeviceLifecycleState GetLifecycle() const;

private:
    Core::DeviceConfig _config;
    std::unordered_map<std::string, std::string> _sessionVars;
    mutable std::mutex _mutex;

    // Resilience state
    Core::ResilienceConfig _resilience;
    // CanProceed() must lazily complete the Open->HalfOpen transition under const, so the state fields are mutable
    mutable CircuitState _circuitState;
    int _consecutiveFailures;
    std::chrono::steady_clock::time_point _openedAt;  // when the circuit breaker opened (cooldown-timing start)
    mutable int _halfOpenProbes;

    // Device lifecycle (KI-04) - orthogonal to the circuit breaker: describes "whether the device is usable",
    // not "whether a request is let through". See DeviceLifecycle.hpp for details
    DeviceLifecycleState _lifecycle;
};

}} // namespace MyProt::Service

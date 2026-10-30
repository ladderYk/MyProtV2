// src/Service/include/MyProt/Service/DeviceLifecycle.hpp
// Device lifecycle state machine (KI-04)
//
// Design points - orthogonal to the circuit breaker:
//   * The circuit breaker (SessionContext::CircuitState) is a transient traffic-layer permission, affecting
//     "whether a request is let through", and is recoverable (HalfOpen probe closes it).
//   * The lifecycle (DeviceLifecycleState) is a device-level persistent dimension, expressing
//     "whether the device is currently usable / what it is doing", triggered by connect/handshake/reconnect/hot-reload.
//   * Both coexist: a Degraded device's breaker may still be Closed, and under an Open breaker a device can still be Connected
//     (just no traffic passing through). No cancel/replace relationship exists.
//
// State-machine transition table (modules/04_Service.md §3, ADR-0004 §6):
//
//                  ┌────────── Disabled (enabled=false at construction, handshake BuildError,
//                  │            or N consecutive Degraded rounds reaching
//                  │            resilience.disableAfterDegraded)
//                  ▲
//   New ──(GOC)──► Connecting ──(handshake+connect OK)──► Connected
//                    │                                │
//                    │                                │(conn-level fault / channel dead)
//                    │                                ▼
//                    └─(connect/handshake fail)──► Degraded ◄──┘
//                                                  │
//                              (next GOC round)────┘
//                                                  │
//                            (reconnect OK)────────► Connected
//
// Trigger table:
//   New -> Connecting            ChannelManager::GetOrCreateChannel entry
//   Connecting -> Connected      PerformConnect OK + handshake sequence complete
//   Connecting -> Degraded       PerformConnect fails or PerformHandshake fails
//                                or handshake template BuildError
//   Connected -> Degraded        PollingEngine marks the whole device segment Bad and it is a connection-level fault
//                                (network/timeout/channel dead) - protocol-level does not transition
//   Degraded -> Connecting       the next-round timer triggers GOC
//   Degraded -> Connected        GOC completes successfully
//   * -> Disabled                protocol/handshake BuildError (immediate, unrecoverable)
//                                or consecutive Degraded rounds >= resilience.disableAfterDegraded
//                                (0 = never auto-disable)
//   Disabled -> (new session)    config hot reload (ResetDevices rebuild); Disabled is terminal
//                                within one session so a still-broken device cannot flap back
//   (config) -> Disabled         DeviceConfig.enabled = false (constructor pins the state)
//
// Fault classification (on the PollingEngine side):
//   Connection-level (-> Degraded): ConnectFailure / Timeout / NetworkError /
//                        ChannelDead (read/write returns a ConnectionLost class)
//   Protocol-level (does not move the lifecycle): InvalidResponse / ParseError /
//                          ProtocolNotFound / TagNotFound
//   Business-level (does not move the lifecycle): a Bad data value but a response was received
//
// Metrics:
//   gauge   myprot_device_lifecycle_state{device,state}   current state (0/1)
//   counter myprot_device_lifecycle_transitions_total{device,from,to}

#pragma once
#include <string>

namespace MyProt { namespace Service {

/// Device lifecycle state - a persistent dimension, describing "whether the device is usable / what it is doing"
enum class DeviceLifecycleState {
    New = 0,        // registered, never connected
    Connecting = 1, // physical connection/handshake in progress
    Connected = 2,  // physical channel + handshake both established, business usable
    Degraded = 3,   // channel dead or reconnecting, not yet at the permanent-disable threshold
    Disabled = 4    // permanently disabled (enabled=false, unrecoverable build error, or degradation threshold;
                    // recovery only via config hot reload)
};

/// State name (for metric labels / logging)
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

/// gauge value: use a single gauge + state label, or one gauge per state?
/// Here a single gauge per device is used: value = state number, label = device only.
inline int DeviceLifecycleGaugeValue(DeviceLifecycleState s) {
    return static_cast<int>(s);
}

}} // namespace MyProt::Service

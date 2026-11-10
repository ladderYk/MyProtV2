// src/Southbound/include/MyProt/Southbound/SouthboundManager.hpp
// Orchestrates inbound/pulled ingestion: one adapter per device.ingest or device.pull,
// a single write entry (TagValueSink) and a per-binding silence watchdog.
// Threading: owns its io_context + thread (symmetric to Northbound::MqttPublisher);
// Apply/Stop are synchronous on the caller thread, every callback runs on _io.

#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Southbound/IInboundTransport.hpp"
#include "MyProt/Southbound/TagValueSink.hpp"

namespace MyProt { namespace Southbound {

class SouthboundManager {
public:
    explicit SouthboundManager(Polling::LatestValueStore& store);
    ~SouthboundManager();

    /// Rebuild every adapter for the given config generation.
    /// Devices without an ingest/pull block, or without any matching source tag, are skipped.
    void Apply(const std::vector<Core::DeviceConfig>& devices,
               const std::vector<Core::TagDefinition>& tags);

    /// Stop all adapters and join the io thread (idempotent).
    void Stop();

    /// Per-round pull health (deviceId, healthy), invoked on this manager's io thread.
    /// App wires it to the device lifecycle; the Southbound module itself stays unaware
    /// of Service. Devices that also own poll tags are excluded — polling owns their state.
    /// Call before Apply() (startup assembly): adapters started later inherit the handler.
    typedef std::function<void(const std::string&, bool)> HealthHandler;
    void SetHealthHandler(HealthHandler handler);

    bool IsRunning() const { return _running.load(); }
    std::size_t DeviceCount() const { return _deviceCount.load(); }

private:
    struct DeviceBinding {
        std::string deviceId;
        Core::IngestConfig ingest;     // valid when isPull == false
        Core::HttpPullConfig pull;     // valid when isPull == true
        bool isPull;                   // adapter branch: HTTP client vs inbound push
        std::map<std::string, Core::TagDefinition> tags;   // tags owned by this adapter
        std::shared_ptr<IInboundTransport> transport;

        int64_t startedMs;        // watchdog baseline
        int64_t lastFrameMs;      // last accepted frame / pull response that matched a tag
        int64_t lastErrorMs;      // last explicit adapter failure (pull: Bad already spoken)
        bool silentEmitted;       // Uncertain already reported for this silent streak
        bool ownsLifecycle;       // pull binding drives device lifecycle (no poll tags)
        std::map<std::string, int64_t> warnMs;   // log throttle per key

        DeviceBinding()
            : isPull(false), startedMs(0), lastFrameMs(0), lastErrorMs(0),
              silentEmitted(false), ownsLifecycle(true) {}
    };
    typedef std::shared_ptr<DeviceBinding> DeviceBindingPtr;

    void Start(const std::vector<Core::DeviceConfig>& devices,
               const std::vector<Core::TagDefinition>& tags);
    void StartBinding(const DeviceBindingPtr& binding,
                      const std::string& mqttFallbackHost);
    void OnFrame(const DeviceBindingPtr& binding, const InboundFrame& frame);
    void OnError(const DeviceBindingPtr& binding, const std::string& message);
    void MarkFrameAccepted(const DeviceBindingPtr& binding, int64_t timestampMs);
    void EmitBadAll(const DeviceBindingPtr& binding, const std::string& reason,
                    int64_t timestampMs, const std::vector<uint8_t>& raw);
    void ScheduleWatchdog();
    void OnWatchdog();
    void WarnThrottled(const DeviceBindingPtr& binding,
                       const std::string& key,
                       const std::string& message);

    static int MaxSilenceMs(const DeviceBinding& binding);

    Polling::LatestValueStore& _store;
    TagValueSink _sink;
    HealthHandler _onHealth;

    std::unique_ptr<asio::io_context> _io;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > _work;
    std::unique_ptr<asio::steady_timer> _watchdog;
    std::thread _thread;

    std::vector<DeviceBindingPtr> _bindings;
    std::atomic<bool> _running;
    std::atomic<std::size_t> _deviceCount;

    SouthboundManager(const SouthboundManager&) = delete;
    SouthboundManager& operator=(const SouthboundManager&) = delete;
};

}} // namespace MyProt::Southbound

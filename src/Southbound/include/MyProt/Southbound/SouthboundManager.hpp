// src/Southbound/include/MyProt/Southbound/SouthboundManager.hpp
// Orchestrates device-initiated ingestion: one adapter per device.ingest, a single
// write entry (TagValueSink) and a per-device silence watchdog.
// Threading: owns its io_context + thread (symmetric to Northbound::MqttPublisher);
// Apply/Stop are synchronous on the caller thread, every callback runs on _io.

#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstdint>
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

    /// Rebuild every inbound adapter for the given config generation.
    /// Devices without an ingest block, or without any source=push tag, are skipped.
    void Apply(const std::vector<Core::DeviceConfig>& devices,
               const std::vector<Core::TagDefinition>& tags);

    /// Stop all adapters and join the io thread (idempotent).
    void Stop();

    bool IsRunning() const { return _running.load(); }
    std::size_t DeviceCount() const { return _deviceCount.load(); }

private:
    struct DeviceBinding {
        std::string deviceId;
        Core::IngestConfig ingest;
        std::map<std::string, Core::TagDefinition> tags;   // push tags only
        std::shared_ptr<IInboundTransport> transport;

        int64_t startedMs;        // watchdog baseline
        int64_t lastFrameMs;      // last accepted inbound frame
        bool silentEmitted;       // Uncertain already reported for this silent streak
        std::map<std::string, int64_t> warnMs;   // log throttle per key

        DeviceBinding()
            : startedMs(0), lastFrameMs(0), silentEmitted(false) {}
    };
    typedef std::shared_ptr<DeviceBinding> DeviceBindingPtr;

    void Start(const std::vector<Core::DeviceConfig>& devices,
               const std::vector<Core::TagDefinition>& tags);
    void StartBinding(const DeviceBindingPtr& binding,
                      const std::string& mqttFallbackHost);
    void OnFrame(const DeviceBindingPtr& binding, const InboundFrame& frame);
    void OnError(const DeviceBindingPtr& binding, const std::string& message);
    void ScheduleWatchdog();
    void OnWatchdog();
    void WarnThrottled(const DeviceBindingPtr& binding,
                       const std::string& key,
                       const std::string& message);

    static int MaxSilenceMs(const Core::IngestConfig& cfg);

    Polling::LatestValueStore& _store;
    TagValueSink _sink;

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

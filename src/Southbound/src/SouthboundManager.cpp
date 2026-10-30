// src/Southbound/src/SouthboundManager.cpp

#include "MyProt/Southbound/SouthboundManager.hpp"

#include <chrono>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Southbound/MqttSubscriber.hpp"
#include "MyProt/Southbound/PayloadMapper.hpp"
#include "MyProt/Southbound/TcpIngestServer.hpp"

namespace MyProt { namespace Southbound {

namespace {

const int kWatchdogTickMs = 500;
const int kWarnThrottleMs = 5000;

std::vector<uint8_t> ToBytes(const std::string& text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

} // namespace

SouthboundManager::SouthboundManager(Polling::LatestValueStore& store)
    : _store(store)
    , _sink([&store](const std::vector<Core::TagValue>& values) {
            store.Update(values);
        })
    , _running(false)
    , _deviceCount(0) {}

SouthboundManager::~SouthboundManager() {
    Stop();
}

void SouthboundManager::Apply(const std::vector<Core::DeviceConfig>& devices,
                              const std::vector<Core::TagDefinition>& tags) {
    Stop();
    Start(devices, tags);
}

void SouthboundManager::Start(const std::vector<Core::DeviceConfig>& devices,
                              const std::vector<Core::TagDefinition>& tags) {
    // Group push tags per device first: a device only gets an adapter when it
    // actually owns source=push tags (otherwise the port/subscription is wasted).
    std::map<std::string, std::vector<const Core::TagDefinition*> > pushTags;
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].source != "push") continue;
        pushTags[tags[i].deviceId].push_back(&tags[i]);
    }

    _io.reset(new asio::io_context());
    _work.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        _io->get_executor()));
    _watchdog.reset(new asio::steady_timer(*_io));
    _bindings.clear();

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const Core::DeviceConfig& device = devices[i];
        if (!device.ingest.has_value()) continue;
        const Core::IngestConfig& ingest = device.ingest.value();
        if (ingest.type != "mqtt" && ingest.type != "tcp") {
            LOG_WARN("Southbound", "设备 %s 的 ingest.type=%s 非法, 跳过",
                     device.id.c_str(), ingest.type.c_str());
            continue;
        }

        std::map<std::string, std::vector<const Core::TagDefinition*> >::const_iterator
            owned = pushTags.find(device.id);
        if (owned == pushTags.end() || owned->second.empty()) {
            LOG_WARN("Southbound",
                     "设备 %s 配置了 ingest 但没有 source=push 标签, 跳过",
                     device.id.c_str());
            continue;
        }

        DeviceBindingPtr binding(new DeviceBinding());
        binding->deviceId = device.id;
        binding->ingest = ingest;
        for (std::size_t k = 0; k < owned->second.size(); ++k) {
            binding->tags[owned->second[k]->name] = *owned->second[k];
        }
        const int64_t now = TagValueSink::NowMs();
        binding->startedMs = now;
        binding->lastFrameMs = now;      // silence is measured from startup as well

        StartBinding(binding, device.connection.host);
        _bindings.push_back(binding);
    }

    _deviceCount.store(_bindings.size());
    if (_bindings.empty()) {
        _watchdog.reset();
        _work.reset();
        _io.reset();
        return;
    }

    _running.store(true);
    ScheduleWatchdog();          // queued before run() starts: no concurrent access
    _thread = std::thread([this]() {
        try {
            _io->run();
        } catch (const std::exception& e) {
            LOG_ERROR("Southbound", "IO 线程异常: %s", e.what());
        } catch (...) {
            LOG_ERROR("Southbound", "IO 线程未知异常");
        }
    });

    LOG_INFO("Southbound", "南向入站已启动: %u 个设备适配器",
             static_cast<unsigned>(_bindings.size()));
}

void SouthboundManager::StartBinding(const DeviceBindingPtr& binding,
                                     const std::string& mqttFallbackHost) {
    if (binding->ingest.type == "mqtt") {
        binding->transport = std::make_shared<MqttSubscriber>(
            *_io, binding->deviceId, binding->ingest.mqtt, mqttFallbackHost);
    } else {
        binding->transport = std::make_shared<TcpIngestServer>(
            *_io, binding->deviceId, binding->ingest.tcp);
    }

    DeviceBindingPtr self = binding;
    binding->transport->Start(
        [this, self](const InboundFrame& frame) { OnFrame(self, frame); },
        [this, self](const std::string& message) { OnError(self, message); });

    if (!binding->transport->IsRunning()) {
        LOG_WARN("Southbound", "设备 %s 的入站适配器启动失败",
                 binding->deviceId.c_str());
    }
}

void SouthboundManager::Stop() {
    _running.store(false);
    if (_io) _io->stop();
    if (_thread.joinable()) _thread.join();

    // The io thread is gone, so the adapters can be released without a race.
    for (std::size_t i = 0; i < _bindings.size(); ++i) {
        if (_bindings[i]->transport) _bindings[i]->transport->Stop();
    }
    _bindings.clear();
    _sink.Reset();
    _watchdog.reset();
    _work.reset();
    _io.reset();
    _deviceCount.store(0);
}

void SouthboundManager::OnFrame(const DeviceBindingPtr& binding,
                                const InboundFrame& frame) {
    const int64_t now = frame.timestampMs > 0 ? frame.timestampMs
                                              : TagValueSink::NowMs();
    binding->lastFrameMs = now;
    if (binding->silentEmitted) {
        binding->silentEmitted = false;
        LOG_INFO("Southbound", "入站恢复 device=%s", binding->deviceId.c_str());
    }

    std::string defaultTag;
    std::string tagField, valueField, tsField;
    if (binding->ingest.type == "mqtt") {
        tagField = binding->ingest.mqtt.tagField;
        valueField = binding->ingest.mqtt.valueField;
        tsField = binding->ingest.mqtt.tsField;
        if (binding->ingest.mqtt.tagFrom == "topic") {
            if (!PayloadMapper::MapTopicTag(frame.topic,
                                            binding->ingest.mqtt.tagTopicIndex,
                                            defaultTag)) {
                WarnThrottled(binding, "topic",
                              "topic 无法映射标签 device=" + binding->deviceId
                                  + " topic=" + frame.topic
                                  + " index=" + std::to_string(
                                        binding->ingest.mqtt.tagTopicIndex));
                return;
            }
        }
    } else if (binding->ingest.type == "tcp") {
        tagField = binding->ingest.tcp.tagField;
        valueField = binding->ingest.tcp.valueField;
        tsField = binding->ingest.tcp.tsField;
        // No topic on a TCP listener: the tag must come from the payload.
    } else {
        return;
    }

    std::vector<MappingItem> items;
    std::string err;
    if (!PayloadMapper::MapJson(frame.payload, tagField, valueField, tsField,
                                defaultTag, items, err)) {
        WarnThrottled(binding, "payload",
                      "payload 解码失败 device=" + binding->deviceId + ": " + err);
        if (!defaultTag.empty()) {
            std::map<std::string, Core::TagDefinition>::const_iterator tag =
                binding->tags.find(defaultTag);
            if (tag != binding->tags.end()) {
                _sink.EmitBad(tag->second, err, now, ToBytes(frame.payload));
            }
        }
        return;
    }

    const std::vector<uint8_t> raw = ToBytes(frame.payload);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const MappingItem& item = items[i];
        std::map<std::string, Core::TagDefinition>::const_iterator tag =
            binding->tags.find(item.tagName);
        if (tag == binding->tags.end()) {
            WarnThrottled(binding, "tag:" + item.tagName,
                          "未声明的 push 标签 device=" + binding->deviceId
                              + " tag=" + item.tagName);
            continue;
        }
        const int64_t ts = item.timestampMs > 0 ? item.timestampMs : now;
        Core::TypedValue value;
        if (!PayloadMapper::ToTypedValue(item.value, tag->second.finalType,
                                         value, err)) {
            _sink.EmitBad(tag->second, err, ts, raw);
            continue;
        }
        _sink.Emit(tag->second, value, ts, raw);
    }
}

void SouthboundManager::OnError(const DeviceBindingPtr& binding,
                                const std::string& message) {
    LOG_WARN("Southbound", "入站适配器错误 device=%s: %s",
             binding->deviceId.c_str(), message.c_str());
}

void SouthboundManager::ScheduleWatchdog() {
    if (!_running.load() || !_watchdog) return;
    _watchdog->expires_after(std::chrono::milliseconds(kWatchdogTickMs));
    _watchdog->async_wait([this](const asio::error_code& ec) {
        if (ec || !_running.load()) return;
        OnWatchdog();
        ScheduleWatchdog();
    });
}

void SouthboundManager::OnWatchdog() {
    const int64_t now = TagValueSink::NowMs();
    for (std::size_t i = 0; i < _bindings.size(); ++i) {
        DeviceBindingPtr& binding = _bindings[i];
        const int maxSilence = MaxSilenceMs(binding->ingest);
        if (maxSilence <= 0 || binding->silentEmitted) continue;
        if (now - binding->lastFrameMs < maxSilence) continue;

        for (std::map<std::string, Core::TagDefinition>::const_iterator
                it = binding->tags.begin(); it != binding->tags.end(); ++it) {
            _sink.EmitSilent(it->second, now);
        }
        binding->silentEmitted = true;
        LOG_WARN("Southbound",
                 "入站静默超时 device=%s 超过 %d ms 无帧, %u 个标签置 Uncertain",
                 binding->deviceId.c_str(), maxSilence,
                 static_cast<unsigned>(binding->tags.size()));
    }
}

void SouthboundManager::WarnThrottled(const DeviceBindingPtr& binding,
                                      const std::string& key,
                                      const std::string& message) {
    const int64_t now = TagValueSink::NowMs();
    std::map<std::string, int64_t>::const_iterator it = binding->warnMs.find(key);
    if (it != binding->warnMs.end() && now - it->second < kWarnThrottleMs) {
        return;
    }
    binding->warnMs[key] = now;
    LOG_WARN("Southbound", "%s", message.c_str());
}

int SouthboundManager::MaxSilenceMs(const Core::IngestConfig& cfg) {
    if (cfg.type == "mqtt") return cfg.mqtt.maxSilenceMs;
    if (cfg.type == "tcp") return cfg.tcp.maxSilenceMs;
    return 0;
}

}} // namespace MyProt::Southbound

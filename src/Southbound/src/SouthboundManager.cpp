// src/Southbound/src/SouthboundManager.cpp

#include "MyProt/Southbound/SouthboundManager.hpp"

#include <chrono>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Southbound/HttpPuller.hpp"
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

void SouthboundManager::SetHealthHandler(HealthHandler handler) {
    _onHealth = handler;
    // 已启动的 pull 适配器同步继承 (未启动时由 StartBinding 注入)。
    for (std::size_t i = 0; i < _bindings.size(); ++i) {
        const DeviceBindingPtr& b = _bindings[i];
        if (!b->isPull || !b->ownsLifecycle) continue;
        std::shared_ptr<HttpPuller> puller =
            std::dynamic_pointer_cast<HttpPuller>(b->transport);
        if (puller) puller->SetHealthHandler(handler);
    }
}

void SouthboundManager::Start(const std::vector<Core::DeviceConfig>& devices,
                              const std::vector<Core::TagDefinition>& tags) {
    // Group tags per device and per adapter branch first: an adapter is only created
    // when the device actually owns tags of that source (otherwise it is wasted work).
    std::map<std::string, std::vector<const Core::TagDefinition*> > pushTags;
    std::map<std::string, std::vector<const Core::TagDefinition*> > pullTags;
    std::map<std::string, int> pollTagCount;   // 同一设备是否还有轮询标签
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].source == "push") {
            pushTags[tags[i].deviceId].push_back(&tags[i]);
        } else if (tags[i].source == "pull") {
            pullTags[tags[i].deviceId].push_back(&tags[i]);
        } else {
            ++pollTagCount[tags[i].deviceId];
        }
    }

    _io.reset(new asio::io_context());
    _work.reset(new asio::executor_work_guard<asio::io_context::executor_type>(
        _io->get_executor()));
    _watchdog.reset(new asio::steady_timer(*_io));
    _bindings.clear();

    for (std::size_t i = 0; i < devices.size(); ++i) {
        const Core::DeviceConfig& device = devices[i];
        // KI-04: 停用设备不建订阅/不占端口/不发请求（与轮询侧同一开关）。
        if (!device.enabled) {
            LOG_INFO("Southbound", "设备 %s 已停用 (enabled=false), 跳过入站绑定",
                     device.id.c_str());
            continue;
        }

        // ── branch 1: HTTP pull (ADR-0023) ──
        if (device.pull.has_value()) {
            const Core::PullConfig& pull = device.pull.value();
            if (pull.type != "http") {
                LOG_WARN("Southbound", "设备 %s 的 pull.type=%s 非法, 跳过",
                         device.id.c_str(), pull.type.c_str());
            } else {
                std::map<std::string, std::vector<const Core::TagDefinition*> >::const_iterator
                    owned = pullTags.find(device.id);
                if (owned == pullTags.end() || owned->second.empty()) {
                    LOG_WARN("Southbound",
                             "设备 %s 配置了 pull 但没有 source=pull 标签, 跳过",
                             device.id.c_str());
                } else {
                    DeviceBindingPtr binding(new DeviceBinding());
                    binding->deviceId = device.id;
                    binding->isPull = true;
                    binding->pull = pull.http;
                    // 轮询与拉取并存的设备: 生命周期归轮询侧, 避免两路互相覆盖状态。
                    binding->ownsLifecycle =
                        (pollTagCount.find(device.id) == pollTagCount.end());
                    for (std::size_t k = 0; k < owned->second.size(); ++k) {
                        binding->tags[owned->second[k]->name] = *owned->second[k];
                    }
                    const int64_t now = TagValueSink::NowMs();
                    binding->startedMs = now;
                    binding->lastFrameMs = now;
                    StartBinding(binding, device.connection.host);
                    _bindings.push_back(binding);
                }
            }
        }

        // ── branch 2: device-initiated ingest (ADR-0019) ──
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
    if (binding->isPull) {
        std::shared_ptr<HttpPuller> puller = std::make_shared<HttpPuller>(
            *_io, binding->deviceId, binding->pull);
        if (_onHealth && binding->ownsLifecycle) {
            puller->SetHealthHandler(_onHealth);
        }
        binding->transport = puller;
    } else if (binding->ingest.type == "mqtt") {
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
    // pull 的静默基线只在"确实命中本设备标签"时推进: HTTP 持续成功但响应里
    // 没有目标标签, 仍应在 maxSilenceMs 后报 Uncertain (ADR-0023 §2.5).
    if (!binding->isPull) MarkFrameAccepted(binding, now);

    std::string defaultTag;
    std::string tagField, valueField, tsField;
    bool arrayAsItems = false;
    if (binding->isPull) {
        tagField = binding->pull.tagField;
        valueField = binding->pull.valueField;
        tsField = binding->pull.tsField;
        arrayAsItems = binding->pull.arrayOfItems;
    } else if (binding->ingest.type == "mqtt") {
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

    const std::vector<uint8_t> raw = ToBytes(frame.payload);
    std::vector<MappingItem> items;
    std::string err;
    if (!PayloadMapper::MapJson(frame.payload, tagField, valueField, tsField,
                                defaultTag, items, err, arrayAsItems)) {
        WarnThrottled(binding, "payload",
                      "payload 解码失败 device=" + binding->deviceId + ": " + err);
        if (binding->isPull) {
            // A failed HTTP round cannot be attributed to one tag: mark them all Bad.
            binding->lastErrorMs = now;   // explicit failure: Bad wins over Uncertain
            EmitBadAll(binding, err, now, raw);
        } else if (!defaultTag.empty()) {
            std::map<std::string, Core::TagDefinition>::const_iterator tag =
                binding->tags.find(defaultTag);
            if (tag != binding->tags.end()) {
                _sink.EmitBad(tag->second, err, now, raw);
            }
        }
        return;
    }
    bool matched = false;   // pull: 本次响应至少命中一个本设备标签
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
        matched = true;
        const int64_t ts = item.timestampMs > 0 ? item.timestampMs : now;
        Core::TypedValue value;
        if (!PayloadMapper::ToTypedValue(item.value, tag->second.finalType,
                                         value, err)) {
            _sink.EmitBad(tag->second, err, ts, raw);
        } else {
            _sink.Emit(tag->second, value, ts, raw);
        }
    }
    // 命中过本设备标签才算一次有效拉取(类型转换失败也算: Bad 已表达故障).
    if (binding->isPull && matched) MarkFrameAccepted(binding, now);
}

void SouthboundManager::MarkFrameAccepted(const DeviceBindingPtr& binding,
                                          int64_t timestampMs) {
    binding->lastFrameMs = timestampMs;
    if (binding->silentEmitted) {
        binding->silentEmitted = false;
        LOG_INFO("Southbound", "入站恢复 device=%s", binding->deviceId.c_str());
    }
}

void SouthboundManager::OnError(const DeviceBindingPtr& binding,
                                const std::string& message) {
    WarnThrottled(binding, "adapter",
                  "入站适配器错误 device=" + binding->deviceId + ": " + message);
    if (binding->isPull) {
        // Transport / status failures carry no per-tag attribution.
        binding->lastErrorMs = TagValueSink::NowMs();
        EmitBadAll(binding, message, binding->lastErrorMs, std::vector<uint8_t>());
    }
}

void SouthboundManager::EmitBadAll(const DeviceBindingPtr& binding,
                                   const std::string& reason,
                                   int64_t timestampMs,
                                   const std::vector<uint8_t>& raw) {
    for (std::map<std::string, Core::TagDefinition>::const_iterator
            it = binding->tags.begin(); it != binding->tags.end(); ++it) {
        _sink.EmitBad(it->second, reason, timestampMs, raw);
    }
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
        const int maxSilence = MaxSilenceMs(*binding);
        if (maxSilence <= 0 || binding->silentEmitted) continue;
        // pull: 最近一次交互是明确失败 → 质量已由 Bad 表达, 不再叠加 Uncertain
        // (否则同一段故障会出现 Bad→Uncertain→Bad 的抖动).
        if (binding->isPull && binding->lastErrorMs > binding->lastFrameMs) continue;
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

int SouthboundManager::MaxSilenceMs(const DeviceBinding& binding) {
    if (binding.isPull) return binding.pull.maxSilenceMs;
    if (binding.ingest.type == "mqtt") return binding.ingest.mqtt.maxSilenceMs;
    if (binding.ingest.type == "tcp") return binding.ingest.tcp.maxSilenceMs;
    return 0;
}

}} // namespace MyProt::Southbound

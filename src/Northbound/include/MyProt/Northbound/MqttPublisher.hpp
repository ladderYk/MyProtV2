// src/Northbound/include/MyProt/Northbound/MqttPublisher.hpp
// 北向 MQTT 发布器 — 把 LatestValueStore 的最新值按配置节奏推送到 broker
// 线程模型: 独占 io_context + 独立线程; Apply/Stop 可在任意非 IO 线程调用。
// 数据源: LatestValueStore::Snapshot() (线程安全), 发布器只读不写。

#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include "MyProt/Core/MqttConfig.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"

namespace MyProt { namespace Northbound {

class MqttClient;

class MqttPublisher {
public:
    explicit MqttPublisher(Polling::LatestValueStore& store);
    ~MqttPublisher();

    /// 应用配置 — 首次装配与热重载同一入口。
    /// enabled=false → 停止; 配置未变 → 跳过; 其余 → 重启会话。
    void Apply(const Core::MqttConfig& cfg);

    /// 停止并等待线程退出 (幂等)
    void Stop();

    bool IsRunning() const { return _running.load(); }
    uint64_t PublishedCount() const { return _published.load(); }

private:
    void Start(const Core::MqttConfig& cfg);
    void ScheduleTick();
    void OnTick();
    void PublishStatus(const char* status);

    std::string BuildTopic(const Core::TagValue& tv) const;
    std::string BuildPayload(const Core::TagValue& tv) const;
    static std::string BuildSignature(const Core::TagValue& tv);

    static bool SameConfig(const Core::MqttConfig& a, const Core::MqttConfig& b);

    Polling::LatestValueStore& _store;
    Core::MqttConfig _cfg;

    std::unique_ptr<asio::io_context> _io;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type> > _work;
    std::unique_ptr<asio::steady_timer> _tickTimer;
    std::shared_ptr<MqttClient> _client;
    std::thread _thread;

    std::atomic<bool> _running;
    std::atomic<uint64_t> _published;

    // 仅 IO 线程访问: tagName → 上次成功发布的值签名 ("changed" 模式去重)
    std::map<std::string, std::string> _publishedSignatures;

    MqttPublisher(const MqttPublisher&) = delete;
    MqttPublisher& operator=(const MqttPublisher&) = delete;
};

}} // namespace MyProt::Northbound

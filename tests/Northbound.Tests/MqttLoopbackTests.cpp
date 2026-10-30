// tests/Northbound.Tests/MqttLoopbackTests.cpp — 北向 MQTT 端到端测试 (进程内假 broker)
// 覆盖: CONNECT/CONNACK 握手、PUBLISH 落库、PINGREQ 心跳、断线自动重连、
//       MqttPublisher 的快照发布与 "changed" 变化过滤。

#include "MyProt/Northbound/MqttClient.hpp"
#include "MyProt/Northbound/MqttPublisher.hpp"
#include "MiniTest.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using MyProt::Northbound::MqttClient;
using MyProt::Northbound::MqttPublisher;

struct BrokerMessage {
    std::string topic;
    std::string payload;
    int qos;
};

/// 极简 MQTT 3.1.1 broker: 只做 CONNECT/CONNACK + 接收 PUBLISH + PINGREQ/PINGRESP
class FakeBroker {
public:
    FakeBroker()
        : _acceptor(_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          _connections(0),
          _pings(0),
          _stopping(false),
          _closeAfterFirstPublish(false),
          _droppedOnce(false) {}

    ~FakeBroker() { Stop(); }

    unsigned short Port() const { return _acceptor.local_endpoint().port(); }

    void Start() { _thread = std::thread([this]() { AcceptLoop(); }); }

    void Stop() {
        _stopping.store(true);
        asio::error_code ec;
        _acceptor.close(ec);
        if (_thread.joinable()) _thread.join();
    }

    /// 收到第一条 PUBLISH 后主动断开 (用于验证客户端自动重连)
    void CloseAfterFirstPublish() { _closeAfterFirstPublish.store(true); }

    int ConnectionCount() const { return _connections.load(); }
    int PingCount() const { return _pings.load(); }

    std::size_t CountTopic(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(_mtx);
        std::size_t n = 0;
        for (std::size_t i = 0; i < _messages.size(); ++i) {
            if (_messages[i].topic == topic) ++n;
        }
        return n;
    }

    BrokerMessage FirstOfTopic(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(_mtx);
        for (std::size_t i = 0; i < _messages.size(); ++i) {
            if (_messages[i].topic == topic) return _messages[i];
        }
        return BrokerMessage();
    }

private:
    static bool ReadPacket(asio::ip::tcp::socket& sock,
                           uint8_t& header,
                           std::vector<uint8_t>& body) {
        asio::error_code ec;
        asio::read(sock, asio::buffer(&header, 1), ec);
        if (ec) return false;

        uint32_t remaining = 0;
        uint32_t multiplier = 1;
        for (;;) {
            uint8_t b = 0;
            asio::read(sock, asio::buffer(&b, 1), ec);
            if (ec) return false;
            remaining += static_cast<uint32_t>(b & 0x7F) * multiplier;
            if ((b & 0x80) == 0) break;
            multiplier *= 128;
            if (multiplier > 128u * 128u * 128u) return false;
        }
        body.assign(remaining, 0);
        if (remaining > 0) {
            asio::read(sock, asio::buffer(&body[0], remaining), ec);
            if (ec) return false;
        }
        return true;
    }

    static bool WritePacket(asio::ip::tcp::socket& sock,
                            const std::vector<uint8_t>& packet) {
        asio::error_code ec;
        asio::write(sock, asio::buffer(packet), ec);
        return !ec;
    }

    void AcceptLoop() {
        while (!_stopping.load()) {
            asio::ip::tcp::socket sock(_io);
            asio::error_code ec;
            _acceptor.accept(sock, ec);
            if (ec) break;
            ++_connections;
            Serve(sock);
        }
    }

    void Serve(asio::ip::tcp::socket& sock) {
        uint8_t header = 0;
        std::vector<uint8_t> body;
        if (!ReadPacket(sock, header, body)) return;
        if (((header >> 4) & 0x0F) != 1) return;   // 只接受 CONNECT

        const uint8_t connack[4] = { 0x20, 0x02, 0x00, 0x00 };
        if (!WritePacket(sock, std::vector<uint8_t>(connack, connack + 4))) return;

        while (true) {
            if (!ReadPacket(sock, header, body)) return;
            const int type = (header >> 4) & 0x0F;
            const int qos = (header >> 1) & 0x03;

            if (type == 3) {   // PUBLISH
                if (body.size() < 2) return;
                const std::size_t topicLen =
                    (static_cast<std::size_t>(body[0]) << 8) | body[1];
                if (2 + topicLen > body.size()) return;
                std::size_t pos = 2 + topicLen;
                uint16_t packetId = 0;
                if (qos > 0) {
                    if (pos + 2 > body.size()) return;
                    packetId = static_cast<uint16_t>(
                        (static_cast<uint16_t>(body[pos]) << 8) | body[pos + 1]);
                    pos += 2;
                }
                BrokerMessage msg;
                msg.topic.assign(reinterpret_cast<const char*>(&body[2]), topicLen);
                msg.payload.assign(
                    reinterpret_cast<const char*>(&body[pos]), body.size() - pos);
                msg.qos = qos;
                {
                    std::lock_guard<std::mutex> lock(_mtx);
                    _messages.push_back(msg);
                }
                if (qos == 1) {
                    const uint8_t puback[4] = {
                        0x40, 0x02,
                        static_cast<uint8_t>((packetId >> 8) & 0xFF),
                        static_cast<uint8_t>(packetId & 0xFF)
                    };
                    WritePacket(sock, std::vector<uint8_t>(puback, puback + 4));
                }
                if (_closeAfterFirstPublish.load() && !_droppedOnce.load()) {
                    _droppedOnce.store(true);
                    return;   // socket 析构 → 客户端读 EOF → 触发重连
                }
            } else if (type == 12) {   // PINGREQ
                ++_pings;
                const uint8_t pingresp[2] = { 0xD0, 0x00 };
                WritePacket(sock, std::vector<uint8_t>(pingresp, pingresp + 2));
            } else if (type == 14) {   // DISCONNECT
                return;
            }
        }
    }

    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    std::thread _thread;

    mutable std::mutex _mtx;
    std::vector<BrokerMessage> _messages;

    std::atomic<int> _connections;
    std::atomic<int> _pings;
    std::atomic<bool> _stopping;
    std::atomic<bool> _closeAfterFirstPublish;
    std::atomic<bool> _droppedOnce;
};

template <typename Pred>
bool WaitFor(Pred pred, int timeoutMs) {
    const int steps = timeoutMs / 10;
    for (int i = 0; i < steps; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

MyProt::Core::MqttConfig LoopbackConfig(unsigned short port,
                                        const std::string& clientId) {
    MyProt::Core::MqttConfig cfg;
    cfg.enabled = true;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.clientId = clientId;
    cfg.keepAliveSec = 1;
    cfg.topicPrefix = "myprot";
    cfg.publishMode = "changed";
    cfg.publishIntervalMs = 100;
    cfg.qos = 0;
    cfg.retain = false;
    return cfg;
}

} // namespace

TEST(MqttClientTest, ConnectsPublishesAndKeepsAlive) {
    FakeBroker broker;
    broker.Start();

    asio::io_context io;
    std::shared_ptr<MqttClient> client(
        new MqttClient(io, LoopbackConfig(broker.Port(), "test-client")));

    client->Start();
    std::thread ioThread([&io]() { io.run(); });

    ASSERT_TRUE(WaitFor([&client]() { return client->IsConnected(); }, 8000));

    ASSERT_TRUE(client->Publish("test/topic", "hello", 0, false));
    EXPECT_TRUE(WaitFor([&broker]() { return broker.CountTopic("test/topic") == 1; }, 5000));

    const BrokerMessage msg = broker.FirstOfTopic("test/topic");
    EXPECT_EQ(msg.topic, std::string("test/topic"));
    EXPECT_EQ(msg.payload, std::string("hello"));

    // keepalive=1s: 应发出 PINGREQ 并收到 PINGRESP, 连接保持
    EXPECT_TRUE(WaitFor([&broker]() { return broker.PingCount() >= 1; }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    EXPECT_TRUE(client->IsConnected());

    client->Stop();
    io.stop();
    ioThread.join();
}

TEST(MqttClientTest, ReconnectsAfterBrokerDrop) {
    FakeBroker broker;
    broker.CloseAfterFirstPublish();
    broker.Start();

    asio::io_context io;
    std::shared_ptr<MqttClient> client(
        new MqttClient(io, LoopbackConfig(broker.Port(), "test-reconnect")));

    client->Start();
    std::thread ioThread([&io]() { io.run(); });

    ASSERT_TRUE(WaitFor([&client]() { return client->IsConnected(); }, 8000));
    ASSERT_TRUE(WaitFor([&broker]() { return broker.ConnectionCount() == 1; }, 5000));

    // 第一条 PUBLISH 后 broker 主动断开 → 客户端按 1s 退避重连
    ASSERT_TRUE(client->Publish("r/1", "one", 0, false));
    ASSERT_TRUE(WaitFor([&broker]() { return broker.CountTopic("r/1") == 1; }, 5000));
    ASSERT_TRUE(WaitFor([&broker]() { return broker.ConnectionCount() >= 2; }, 8000));
    ASSERT_TRUE(WaitFor([&client]() { return client->IsConnected(); }, 8000));

    EXPECT_TRUE(client->Publish("r/1", "two", 0, false));
    EXPECT_TRUE(WaitFor([&broker]() { return broker.CountTopic("r/1") == 2; }, 5000));

    client->Stop();
    io.stop();
    ioThread.join();
}

TEST(MqttPublisherTest, PublishesLatestValuesAndFiltersUnchanged) {
    FakeBroker broker;
    broker.Start();

    MyProt::Polling::LatestValueStore store;
    std::vector<MyProt::Core::TagValue> values(1);
    values[0].tagName = "Coil0";
    values[0].deviceId = "DEV";
    values[0].typedValue.type = MyProt::Core::ValueType::Bool;
    values[0].typedValue.b = true;
    values[0].quality = MyProt::Core::QualityCode::Good;
    values[0].timestamp = 1000;
    values[0].requestId = 42;
    values[0].valueChanged = true;
    store.Update(values);

    MqttPublisher publisher(store);
    publisher.Apply(LoopbackConfig(broker.Port(), "test-publisher"));

    // 连接后先发 retained 状态, 再全量补发一次当前值
    ASSERT_TRUE(WaitFor([&broker]() {
        return broker.CountTopic("myprot/DEV/Coil0") == 1;
    }, 8000));
    EXPECT_TRUE(broker.CountTopic("myprot/status") >= 1);

    const BrokerMessage first = broker.FirstOfTopic("myprot/DEV/Coil0");
    EXPECT_TRUE(first.payload.find("\"value\":true") != std::string::npos);
    EXPECT_TRUE(first.payload.find("\"quality\":\"Good\"") != std::string::npos);
    EXPECT_TRUE(first.payload.find("\"requestId\":42") != std::string::npos);

    // 同值 + 新时间戳: "changed" 模式应跳过 (时间戳不参与变化判定)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::size_t before = broker.CountTopic("myprot/DEV/Coil0");
    values[0].timestamp = 2000;
    values[0].valueChanged = false;
    store.Update(values);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(broker.CountTopic("myprot/DEV/Coil0"), before);

    // 值变化 → 再发一条
    values[0].typedValue.b = false;
    values[0].timestamp = 3000;
    values[0].valueChanged = true;
    store.Update(values);
    EXPECT_TRUE(WaitFor([&broker, before]() {
        return broker.CountTopic("myprot/DEV/Coil0") == before + 1;
    }, 5000));

    publisher.Stop();
}

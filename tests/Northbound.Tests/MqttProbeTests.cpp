// tests/Northbound.Tests/MqttProbeTests.cpp — MQTT 观测会话 (管理面 MQTT 测试面板后端) 测试
// 覆盖: 入参校验 / 过滤器与 UTF-8 规则 / broker 地址归一化 / 订阅收消息 /
//       二进制载荷 hex / 借会话发布 / 会话数上限 / 空闲回收 / 停止语义。
// 使用进程内极简 broker (CONNECT/SUBSCRIBE/PUBLISH/PINGREQ), 不依赖外部服务。

#include "MyProt/Northbound/MqttProbeHub.hpp"

#include "MiniTest.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using MyProt::Northbound::MqttProbeHub;
using MyProt::Northbound::MqttProbeOptions;
using MyProt::Northbound::MqttProbeSpec;

template <typename Pred>
bool WaitFor(Pred pred, int timeoutMs) {
    const int steps = timeoutMs / 10;
    for (int i = 0; i < steps; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/// 极简订阅侧 broker: CONNECT/CONNACK + SUBSCRIBE/SUBACK + PINGREQ/PINGRESP,
/// 并把测试线程推送的 PUBLISH 写到客户端 socket。
class SubBroker {
public:
    SubBroker()
        : _acceptor(_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          _stopping(false), _subscribeCount(0) {}

    ~SubBroker() { Stop(); }

    unsigned short Port() const { return _acceptor.local_endpoint().port(); }

    void Start() { _thread = std::thread([this]() { AcceptLoop(); }); }

    void Stop() {
        if (_stopping.exchange(true)) return;
        // Linux: close() 一个正被阻塞 accept 的监听 socket 不会唤醒 accept
        // (Windows 会), 直接 join 会挂死。先自连一次把 accept 拉出阻塞。
        if (_acceptor.is_open()) {
            asio::error_code wakeEc;
            asio::ip::tcp::endpoint local = _acceptor.local_endpoint(wakeEc);
            if (!wakeEc) {
                asio::ip::tcp::socket wake(_io);
                wake.connect(local, wakeEc);          // 失败也无妨, 下面 close 兑底
                wake.close(wakeEc);
            }
        }
        asio::error_code ec;
        _acceptor.close(ec);
        std::shared_ptr<asio::ip::tcp::socket> client;
        {
            std::lock_guard<std::mutex> lock(_clientMutex);
            client = _client;
            _client.reset();
        }
        if (client) {
            client->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            client->close(ec);
        }
        if (_thread.joinable()) _thread.join();
    }

    int SubscribeCount() const { return _subscribeCount.load(); }

    bool PushPublish(const std::string& topic, const std::string& payload) {
        std::vector<uint8_t> packet;
        BuildPublish(topic, payload, 0, false, packet);

        std::shared_ptr<asio::ip::tcp::socket> client;
        {
            std::lock_guard<std::mutex> lock(_clientMutex);
            client = _client;
        }
        if (!client) return false;
        std::lock_guard<std::mutex> writeLock(_writeMutex);
        asio::error_code ec;
        asio::write(*client, asio::buffer(packet), ec);
        return !ec;
    }

    std::size_t CountTopic(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(_messagesMutex);
        std::size_t n = 0;
        for (std::size_t i = 0; i < _topics.size(); ++i) {
            if (_topics[i] == topic) ++n;
        }
        return n;
    }

    std::string FirstPayloadOf(const std::string& topic) const {
        std::lock_guard<std::mutex> lock(_messagesMutex);
        for (std::size_t i = 0; i < _topics.size(); ++i) {
            if (_topics[i] == topic) return _payloads[i];
        }
        return std::string();
    }

private:
    static bool ReadPacket(asio::ip::tcp::socket& sock, uint8_t* header,
                           std::vector<uint8_t>* body) {
        asio::error_code ec;
        asio::read(sock, asio::buffer(header, 1), ec);
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
        body->assign(remaining, 0);
        if (remaining > 0) {
            asio::read(sock, asio::buffer(&(*body)[0], remaining), ec);
            if (ec) return false;
        }
        return true;
    }

    static void AppendRemainingLength(std::size_t value,
                                      std::vector<uint8_t>* out) {
        for (;;) {
            uint8_t digit = static_cast<uint8_t>(value % 128);
            value /= 128;
            if (value > 0) digit |= 0x80;
            out->push_back(digit);
            if (value == 0) break;
        }
    }

    static void BuildPublish(const std::string& topic, const std::string& payload,
                             int qos, bool retain, std::vector<uint8_t>& out) {
        std::vector<uint8_t> body;
        body.push_back(static_cast<uint8_t>((topic.size() >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(topic.size() & 0xFF));
        body.insert(body.end(), topic.begin(), topic.end());
        if (qos > 0) {
            body.push_back(0x00);
            body.push_back(0x01);
        }
        body.insert(body.end(), payload.begin(), payload.end());

        uint8_t flags = static_cast<uint8_t>((qos & 0x03) << 1);
        if (retain) flags |= 0x01;
        out.push_back(static_cast<uint8_t>(0x30 | flags));
        AppendRemainingLength(body.size(), &out);
        out.insert(out.end(), body.begin(), body.end());
    }

    bool Write(asio::ip::tcp::socket& sock, const std::vector<uint8_t>& packet) {
        std::lock_guard<std::mutex> lock(_writeMutex);
        asio::error_code ec;
        asio::write(sock, asio::buffer(packet), ec);
        return !ec;
    }

    void AcceptLoop() {
        while (!_stopping.load()) {
            std::shared_ptr<asio::ip::tcp::socket> sock(
                new asio::ip::tcp::socket(_io));
            asio::error_code ec;
            _acceptor.accept(*sock, ec);
            if (ec) break;
            if (_stopping.load()) break;        // 被 Stop() 的自连唤醒: 退出前不处理
            {
                std::lock_guard<std::mutex> lock(_clientMutex);
                _client = sock;
            }
            Serve(*sock);
            {
                std::lock_guard<std::mutex> lock(_clientMutex);
                if (_client == sock) _client.reset();
            }
        }
    }

    void Serve(asio::ip::tcp::socket& sock) {
        uint8_t header = 0;
        std::vector<uint8_t> body;
        if (!ReadPacket(sock, &header, &body)) return;
        if (((header >> 4) & 0x0F) != 1) return;      // 只接受 CONNECT

        const uint8_t connack[4] = { 0x20, 0x02, 0x00, 0x00 };
        if (!Write(sock, std::vector<uint8_t>(connack, connack + 4))) return;

        for (;;) {
            if (!ReadPacket(sock, &header, &body)) return;
            const int type = (header >> 4) & 0x0F;
            const int qos = (header >> 1) & 0x03;

            if (type == 8) {                            // SUBSCRIBE
                if (body.size() < 3) return;
                const uint8_t suback[5] = {
                    0x90, 0x03, body[0], body[1], 0x00
                };
                if (!Write(sock, std::vector<uint8_t>(suback, suback + 5))) return;
                ++_subscribeCount;
            } else if (type == 3) {                     // PUBLISH
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
                const std::string topic(
                    reinterpret_cast<const char*>(&body[2]), topicLen);
                const std::string payload(
                    reinterpret_cast<const char*>(&body[pos]), body.size() - pos);
                {
                    std::lock_guard<std::mutex> lock(_messagesMutex);
                    _topics.push_back(topic);
                    _payloads.push_back(payload);
                }
                if (qos == 1) {
                    const uint8_t puback[4] = {
                        0x40, 0x02,
                        static_cast<uint8_t>((packetId >> 8) & 0xFF),
                        static_cast<uint8_t>(packetId & 0xFF)
                    };
                    if (!Write(sock, std::vector<uint8_t>(puback, puback + 4))) return;
                }
            } else if (type == 12) {                    // PINGREQ
                const uint8_t pingresp[2] = { 0xD0, 0x00 };
                if (!Write(sock, std::vector<uint8_t>(pingresp, pingresp + 2))) return;
            } else if (type == 14) {                    // DISCONNECT
                return;
            }
        }
    }

    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    std::thread _thread;
    std::atomic<bool> _stopping;

    mutable std::mutex _clientMutex;
    std::shared_ptr<asio::ip::tcp::socket> _client;
    std::mutex _writeMutex;

    mutable std::mutex _messagesMutex;
    std::vector<std::string> _topics;
    std::vector<std::string> _payloads;

    std::atomic<int> _subscribeCount;
};

/// 反复 Poll 直到 collected 中出现 needle; 返回累计帧文本 (供断言定位)
std::string DrainUntil(MqttProbeHub& hub, const std::string& sid,
                       const std::string& needle, int timeoutMs) {
    std::string collected;
    const int steps = timeoutMs / 50;
    for (int i = 0; i < steps; ++i) {
        const std::string frame = hub.Poll(sid);
        if (!frame.empty()) collected += frame;
        if (collected.find(needle) != std::string::npos) return collected;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return collected;
}

MqttProbeSpec MakeSpec(unsigned short port, const std::string& topic) {
    MqttProbeSpec spec;
    spec.host = "127.0.0.1";
    spec.port = static_cast<int>(port);
    spec.topic = topic;
    spec.qos = 0;
    spec.keepAliveSec = 30;
    return spec;
}

} // namespace

TEST(MqttProbeValidationTest, TopicFilterRules) {
    EXPECT_TRUE(MqttProbeHub::IsValidTopicFilter("myprot/#"));
    EXPECT_TRUE(MqttProbeHub::IsValidTopicFilter("#"));
    EXPECT_TRUE(MqttProbeHub::IsValidTopicFilter("sensor/+/temp"));
    EXPECT_TRUE(MqttProbeHub::IsValidTopicFilter("a//b"));
    EXPECT_TRUE(MqttProbeHub::IsValidTopicFilter("exact/topic"));

    EXPECT_FALSE(MqttProbeHub::IsValidTopicFilter(""));
    EXPECT_FALSE(MqttProbeHub::IsValidTopicFilter("a/#/b"));
    EXPECT_FALSE(MqttProbeHub::IsValidTopicFilter("a/b#"));
    EXPECT_FALSE(MqttProbeHub::IsValidTopicFilter("a/+b"));
    EXPECT_FALSE(MqttProbeHub::IsValidTopicFilter("a/#x"));
}

TEST(MqttProbeValidationTest, Utf8Rules) {
    EXPECT_TRUE(MqttProbeHub::IsValidUtf8(""));
    EXPECT_TRUE(MqttProbeHub::IsValidUtf8("plain ascii"));
    EXPECT_TRUE(MqttProbeHub::IsValidUtf8("中文 UTF-8"));
    EXPECT_TRUE(MqttProbeHub::IsValidUtf8("\xE2\x9C\x93"));       // U+2713

    EXPECT_FALSE(MqttProbeHub::IsValidUtf8(std::string("\xFF", 1)));
    EXPECT_FALSE(MqttProbeHub::IsValidUtf8(std::string("\xC0\x80", 2)));  // overlong
    EXPECT_FALSE(MqttProbeHub::IsValidUtf8(std::string("\xE4\xB8", 2)));  // truncated
}

TEST(MqttProbeValidationTest, BrokerNormalization) {
    std::string host = "mqtt://10.0.0.5:1884";
    int port = 0;
    ASSERT_TRUE(MqttProbeHub::NormalizeBroker(&host, &port));
    EXPECT_EQ(host, std::string("10.0.0.5"));
    EXPECT_EQ(port, 1884);

    host = "tcp://broker.local";
    port = 0;
    ASSERT_TRUE(MqttProbeHub::NormalizeBroker(&host, &port));
    EXPECT_EQ(host, std::string("broker.local"));
    EXPECT_EQ(port, 1883);

    host = "  [::1]:1885  ";
    port = 0;
    ASSERT_TRUE(MqttProbeHub::NormalizeBroker(&host, &port));
    EXPECT_EQ(host, std::string("::1"));
    EXPECT_EQ(port, 1885);

    host = "   ";
    port = 0;
    EXPECT_FALSE(MqttProbeHub::NormalizeBroker(&host, &port));

    host = "host with space";
    port = 0;
    EXPECT_FALSE(MqttProbeHub::NormalizeBroker(&host, &port));

    host = "127.0.0.1";
    port = 70000;
    EXPECT_FALSE(MqttProbeHub::NormalizeBroker(&host, &port));
}

TEST(MqttProbeHubTest, RejectsInvalidSpec) {
    MqttProbeHub hub;
    std::string error;

    MqttProbeSpec spec;                         // host 为空
    spec.topic = "myprot/#";
    EXPECT_TRUE(hub.Create(spec, &error).empty());
    EXPECT_TRUE(!error.empty());

    spec = MakeSpec(1883, "a/#/b");             // 非法过滤器
    error.clear();
    EXPECT_TRUE(hub.Create(spec, &error).empty());
    EXPECT_TRUE(error.find("过滤器") != std::string::npos);

    spec = MakeSpec(1883, "myprot/#");
    spec.qos = 2;                               // QoS 越界
    error.clear();
    EXPECT_TRUE(hub.Create(spec, &error).empty());
    EXPECT_TRUE(error.find("QoS") != std::string::npos);

    EXPECT_EQ(hub.Count(), static_cast<std::size_t>(0));
}

TEST(MqttProbeHubTest, ReceivesSubscribedMessages) {
    SubBroker broker;
    broker.Start();

    MqttProbeHub hub;
    std::string error;
    const std::string sid = hub.Create(MakeSpec(broker.Port(), "myprot/#"), &error);
    ASSERT_TRUE(!sid.empty());

    const std::string online = DrainUntil(hub, sid, "\"state\":\"online\"", 8000);
    EXPECT_TRUE(online.find("online") != std::string::npos);
    EXPECT_TRUE(broker.SubscribeCount() >= 1);

    ASSERT_TRUE(broker.PushPublish("myprot/dev/temp", "{\"v\":21}"));
    const std::string frames = DrainUntil(hub, sid, "myprot/dev/temp", 5000);
    EXPECT_TRUE(frames.find("\"type\":\"message\"") != std::string::npos);
    EXPECT_TRUE(frames.find("\"encoding\":\"utf8\"") != std::string::npos);
    EXPECT_TRUE(frames.find("myprot/dev/temp") != std::string::npos);

    hub.Stop(sid);
}

TEST(MqttProbeHubTest, EncodesBinaryPayloadAsHex) {
    SubBroker broker;
    broker.Start();

    MqttProbeHub hub;
    std::string error;
    const std::string sid = hub.Create(MakeSpec(broker.Port(), "bin/#"), &error);
    ASSERT_TRUE(!sid.empty());
    ASSERT_TRUE(!DrainUntil(hub, sid, "\"state\":\"online\"", 8000).empty());

    ASSERT_TRUE(broker.PushPublish("bin/raw", std::string("\x01\xFF", 2)));
    const std::string frames = DrainUntil(hub, sid, "\"payload\":\"01ff\"", 5000);
    EXPECT_TRUE(frames.find("\"encoding\":\"hex\"") != std::string::npos);
    EXPECT_TRUE(frames.find("\"payload\":\"01ff\"") != std::string::npos);

    hub.Stop(sid);
}

TEST(MqttProbeHubTest, PublishesThroughSession) {
    SubBroker broker;
    broker.Start();

    MqttProbeHub hub;
    std::string error;
    const std::string sid = hub.Create(MakeSpec(broker.Port(), "myprot/#"), &error);
    ASSERT_TRUE(!sid.empty());
    ASSERT_TRUE(!DrainUntil(hub, sid, "\"state\":\"online\"", 8000).empty());

    error.clear();
    EXPECT_TRUE(hub.Publish(sid, "probe/out", "hello", 0, false, &error));
    EXPECT_TRUE(WaitFor([&broker]() {
        return broker.CountTopic("probe/out") == 1;
    }, 5000));
    EXPECT_EQ(broker.FirstPayloadOf("probe/out"), std::string("hello"));

    error.clear();
    EXPECT_FALSE(hub.Publish(sid, "probe/#", "bad", 0, false, &error));
    EXPECT_TRUE(error.find("通配符") != std::string::npos);

    hub.Stop(sid);
}

TEST(MqttProbeHubTest, EnforcesSessionLimit) {
    MqttProbeOptions opts;
    opts.maxSessions = 2;
    opts.idleTimeoutMs = 60000;
    MqttProbeHub hub(opts);

    // 未监听的端口: 建会话不阻塞, 连接在后台失败重试
    std::string error;
    const std::string first = hub.Create(MakeSpec(1, "a/#"), &error);
    ASSERT_TRUE(!first.empty());
    const std::string second = hub.Create(MakeSpec(1, "b/#"), &error);
    ASSERT_TRUE(!second.empty());

    error.clear();
    const std::string third = hub.Create(MakeSpec(1, "c/#"), &error);
    EXPECT_TRUE(third.empty());
    EXPECT_TRUE(error.find("同时最多") != std::string::npos);
    EXPECT_EQ(hub.Count(), static_cast<std::size_t>(2));

    hub.StopAll();
    EXPECT_EQ(hub.Count(), static_cast<std::size_t>(0));
}

TEST(MqttProbeHubTest, ReapsIdleSession) {
    MqttProbeOptions opts;
    opts.idleTimeoutMs = 1000;
    opts.statusTickMs = 50;
    MqttProbeHub hub(opts);

    std::string error;
    const std::string sid = hub.Create(MakeSpec(1, "idle/#"), &error);
    ASSERT_TRUE(!sid.empty());
    EXPECT_EQ(hub.Count(), static_cast<std::size_t>(1));

    // 从不 Poll → 超过空闲阈值后被回收
    EXPECT_TRUE(WaitFor([&hub]() {
        return hub.Count() == 0;
    }, 6000));
    EXPECT_TRUE(hub.Status(sid).empty());
}

TEST(MqttProbeHubTest, StopRemovesSession) {
    MqttProbeHub hub;
    std::string error;
    const std::string sid = hub.Create(MakeSpec(1, "stop/#"), &error);
    ASSERT_TRUE(!sid.empty());
    EXPECT_TRUE(!hub.Status(sid).empty());

    EXPECT_TRUE(hub.Stop(sid));
    EXPECT_TRUE(hub.Status(sid).empty());
    EXPECT_FALSE(hub.Stop(sid));
    EXPECT_EQ(hub.Count(), static_cast<std::size_t>(0));
}

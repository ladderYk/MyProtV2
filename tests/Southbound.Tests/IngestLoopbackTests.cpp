// tests/Southbound.Tests/IngestLoopbackTests.cpp — 入站链路端到端测试 (进程内假 broker / 真 socket)
// 覆盖: TCP 监听切帧、MQTT 订阅收包、SouthboundManager 的映射/写入口/静默看门狗。

#include "MyProt/Southbound/MqttSubscriber.hpp"
#include "MyProt/Southbound/SouthboundManager.hpp"
#include "MyProt/Southbound/TcpIngestServer.hpp"
#include "MyProt/Northbound/MqttCodec.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
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

using MyProt::Core::IngestConfig;
using MyProt::Core::MqttIngestConfig;
using MyProt::Core::TcpIngestConfig;
using MyProt::Southbound::InboundFrame;
using MyProt::Southbound::MqttSubscriber;
using MyProt::Southbound::SouthboundManager;
using MyProt::Southbound::TcpIngestServer;

template <typename Pred>
bool WaitFor(Pred pred, int timeoutMs) {
    const int steps = timeoutMs / 10;
    for (int i = 0; i < steps; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/// Reserve a loopback port: bind 0, read the assigned port, release it.
unsigned short ReservePort() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(
        io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    return acceptor.local_endpoint().port();
}

bool SendBytes(unsigned short port, const std::string& payload) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    asio::error_code ec;
    socket.connect(asio::ip::tcp::endpoint(
                       asio::ip::make_address("127.0.0.1"), port), ec);
    if (ec) return false;
    asio::write(socket, asio::buffer(payload), ec);
    return !ec;
}

bool ReadPacket(asio::ip::tcp::socket& sock,
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

/// 极简订阅端 broker: CONNECT/CONNACK → SUBSCRIBE/SUBACK → 推送一条 PUBLISH。
class FakeSubBroker {
public:
    FakeSubBroker()
        : _acceptor(_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          _sock(_io),
          _stopping(false),
          _subscribed(false) {}

    ~FakeSubBroker() { Stop(); }

    unsigned short Port() const { return _acceptor.local_endpoint().port(); }

    void SetPublish(const std::string& topic, const std::string& payload) {
        _publishTopic = topic;
        _publishPayload = payload;
    }

    void Start() { _thread = std::thread([this]() { Serve(); }); }

    void Stop() {
        _stopping.store(true);
        asio::error_code ec;
        _acceptor.close(ec);
        // The accepted socket is closed explicitly: the client may still hold it
        // (a queued handler keeps the client alive), so a bare join could block.
        _sock.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        _sock.close(ec);
        if (_thread.joinable()) _thread.join();
    }

    bool Subscribed() const { return _subscribed.load(); }

private:
    void Serve() {
        asio::ip::tcp::socket& sock = _sock;
        asio::error_code ec;
        _acceptor.accept(sock, ec);
        if (ec) return;

        uint8_t header = 0;
        std::vector<uint8_t> body;
        if (!ReadPacket(sock, header, body)) return;
        if (((header >> 4) & 0x0F) != 1) return;      // CONNECT

        const uint8_t connack[4] = { 0x20, 0x02, 0x00, 0x00 };
        asio::write(sock, asio::buffer(connack, 4), ec);
        if (ec) return;

        while (!_stopping.load()) {
            if (!ReadPacket(sock, header, body)) return;
            const int type = (header >> 4) & 0x0F;

            if (type == 8) {                          // SUBSCRIBE
                if (body.size() < 5) return;
                const uint16_t packetId = static_cast<uint16_t>(
                    (static_cast<uint16_t>(body[0]) << 8) | body[1]);
                _subscribed.store(true);
                const uint8_t suback[5] = {
                    0x90, 0x03,
                    static_cast<uint8_t>((packetId >> 8) & 0xFF),
                    static_cast<uint8_t>(packetId & 0xFF),
                    0x00
                };
                asio::write(sock, asio::buffer(suback, 5), ec);
                if (ec) return;

                const std::vector<uint8_t> publish =
                    MyProt::Northbound::MqttCodec::EncodePublish(
                        _publishTopic, _publishPayload, 0, false, 0);
                asio::write(sock, asio::buffer(publish), ec);
                if (ec) return;
            } else if (type == 12) {                  // PINGREQ
                const uint8_t pingresp[2] = { 0xD0, 0x00 };
                asio::write(sock, asio::buffer(pingresp, 2), ec);
                if (ec) return;
            } else if (type == 14) {                  // DISCONNECT
                return;
            }
        }
    }

    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    asio::ip::tcp::socket _sock;
    std::thread _thread;
    std::string _publishTopic;
    std::string _publishPayload;
    std::atomic<bool> _stopping;
    std::atomic<bool> _subscribed;
};

} // namespace

TEST(IngestTest, TcpListenerSplitsFixedFrames) {
    const std::string frame = "{\"value\":1}";
    const unsigned short port = ReservePort();

    TcpIngestConfig cfg;
    cfg.bindAddress = "127.0.0.1";
    cfg.port = port;
    cfg.framing.type = MyProt::Core::FramingType::Fixed;
    cfg.framing.fixed.fixedLength = static_cast<int>(frame.size());

    asio::io_context io;
    std::shared_ptr<TcpIngestServer> server =
        std::make_shared<TcpIngestServer>(io, "D-TCP", cfg);

    std::mutex mtx;
    std::vector<InboundFrame> frames;
    server->Start(
        [&mtx, &frames](const InboundFrame& f) {
            std::lock_guard<std::mutex> lock(mtx);
            frames.push_back(f);
        },
        [](const std::string&) {});

    std::thread ioThread([&io]() { io.run(); });
    bool connected = WaitFor([port]() {
        asio::io_context probeIo;
        asio::ip::tcp::socket probe(probeIo);
        asio::error_code ec;
        probe.connect(asio::ip::tcp::endpoint(
                          asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }, 2000);
    EXPECT_TRUE(connected);

    EXPECT_TRUE(SendBytes(port, frame + frame));
    EXPECT_TRUE(WaitFor([&mtx, &frames]() {
        std::lock_guard<std::mutex> lock(mtx);
        return frames.size() >= 2;
    }, 2000));

    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mtx);
        count = frames.size();
        if (!frames.empty()) EXPECT_EQ(frames[0].payload, frame);
    }
    EXPECT_EQ(count, static_cast<std::size_t>(2));

    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server->Stop();
}

TEST(IngestTest, MqttSubscriberReceivesPublish) {
    FakeSubBroker broker;
    broker.SetPublish("plant/dev1/data", "{\"value\":9}");
    broker.Start();

    MqttIngestConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = broker.Port();
    cfg.clientId = "sub-test";
    cfg.topic = "plant/dev1/data";
    cfg.qos = 0;

    asio::io_context io;
    MqttSubscriber subscriber(io, "D-MQTT", cfg, "");

    std::mutex mtx;
    std::string topic;
    std::string payload;
    subscriber.Start(
        [&mtx, &topic, &payload](const InboundFrame& f) {
            std::lock_guard<std::mutex> lock(mtx);
            topic = f.topic;
            payload = f.payload;
        },
        [](const std::string&) {});

    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&broker]() { return broker.Subscribed(); }, 3000));
    EXPECT_TRUE(WaitFor([&mtx, &payload]() {
        std::lock_guard<std::mutex> lock(mtx);
        return !payload.empty();
    }, 3000));

    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(topic, std::string("plant/dev1/data"));
        EXPECT_EQ(payload, std::string("{\"value\":9}"));
    }

    io.stop();
    if (ioThread.joinable()) ioThread.join();
    subscriber.Stop();
    broker.Stop();
}

TEST(IngestTest, ManagerMapsPushAndDegradesOnSilence) {
    const std::string frame = "{\"tag\":\"P.Temp\",\"value\":21.5}";
    const unsigned short port = ReservePort();

    MyProt::Core::DeviceConfig device;
    device.id = "P";
    device.connection.host = "127.0.0.1";
    IngestConfig ingest;
    ingest.type = "tcp";
    ingest.tcp.bindAddress = "127.0.0.1";
    ingest.tcp.port = port;
    ingest.tcp.framing.type = MyProt::Core::FramingType::Fixed;
    ingest.tcp.framing.fixed.fixedLength = static_cast<int>(frame.size());
    ingest.tcp.maxSilenceMs = 300;
    ingest.tcp.tagField = "tag";
    ingest.tcp.valueField = "value";
    ingest.tcp.tsField = "ts";
    device.ingest = MyProt::Core::Optional<IngestConfig>(ingest);

    MyProt::Core::TagDefinition tag;
    tag.name = "P.Temp";
    tag.deviceId = "P";
    tag.source = "push";
    tag.finalType = "Double";

    std::vector<MyProt::Core::DeviceConfig> devices(1, device);
    std::vector<MyProt::Core::TagDefinition> tags(1, tag);

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    manager.Apply(devices, tags);
    EXPECT_TRUE(manager.IsRunning());
    EXPECT_EQ(manager.DeviceCount(), static_cast<std::size_t>(1));

    EXPECT_TRUE(WaitFor([port]() {
        asio::io_context probeIo;
        asio::ip::tcp::socket probe(probeIo);
        asio::error_code ec;
        probe.connect(asio::ip::tcp::endpoint(
                          asio::ip::make_address("127.0.0.1"), port), ec);
        return !ec;
    }, 2000));
    EXPECT_TRUE(SendBytes(port, frame));

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("P.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Good
            && value.typedValue.type == MyProt::Core::ValueType::Double
            && value.typedValue.d == 21.5;
    }, 2000));

    // maxSilenceMs=300 -> 静默后质量降级为 Uncertain, 保留上次值
    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("P.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Uncertain;
    }, 3000));

    MyProt::Core::TagValue last;
    ASSERT_TRUE(store.Lookup("P.Temp", last));
    EXPECT_EQ(last.typedValue.d, 21.5);

    manager.Stop();
    EXPECT_FALSE(manager.IsRunning());
}

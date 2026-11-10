// tests/Southbound.Tests/HttpPullTests.cpp — HTTP 拉取适配器测试 (ADR-0023)
// 覆盖: URL 解析、数组响应逐元素映射、真 socket 拉取、失败置 Bad、SouthboundManager 接入。

#include "MyProt/Southbound/HttpPuller.hpp"
#include "MyProt/Southbound/PayloadMapper.hpp"
#include "MyProt/Southbound/SouthboundManager.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MiniTest.hpp"

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using MyProt::Core::HttpPullConfig;
using MyProt::Southbound::HttpPuller;
using MyProt::Southbound::InboundFrame;
using MyProt::Southbound::MappingItem;
using MyProt::Southbound::PayloadMapper;
using MyProt::Southbound::SouthboundManager;

template <typename Pred>
bool WaitFor(Pred pred, int timeoutMs) {
    const int steps = timeoutMs / 10;
    for (int i = 0; i < steps; ++i) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/// 极简 HTTP 服务端: 读掉请求头, 返回固定状态码 + JSON 体。
/// keepAlive=false 时每轮带 Connection: close (v1 默认行为);
/// keepAlive=true 时在同一连接上连续服务多轮。
/// chunked=true 时用 Transfer-Encoding: chunked 分两块发送 body。
class FakeHttpServer {
public:
    FakeHttpServer()
        : _acceptor(_io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          _stopping(false), _status(200), _hits(0), _connections(0),
          _keepAlive(false), _closeAfter(0), _chunked(false),
          _trailers(false), _badChunkSize(false) {}

    ~FakeHttpServer() { Stop(); }

    unsigned short Port() const { return _acceptor.local_endpoint().port(); }

    void SetResponse(int status, const std::string& body) {
        _status = status;
        _body = body;
    }

    void SetKeepAlive(bool on) { _keepAlive.store(on); }

    /// chunked 响应 (可选 trailer 行); badChunkSize=true 时发送非法长度行。
    void SetChunked(bool on, bool trailers = false, bool badChunkSize = false) {
        _chunked.store(on);
        _trailers.store(trailers);
        _badChunkSize.store(badChunkSize);
    }

    /// 每条连接服务 N 个请求后直接关闭 (不发 Connection: close),
    /// 模拟 keep-alive 连接被对端静默回收。
    void SetCloseAfter(int perConnection) { _closeAfter.store(perConnection); }

    void Start() { _thread = std::thread([this]() { Serve(); }); }

    void Stop() {
        _stopping.store(true);
        asio::error_code ec;
        _acceptor.close(ec);
        if (_thread.joinable()) _thread.join();
    }

    int Hits() const { return _hits.load(); }
    int Connections() const { return _connections.load(); }

    /// 最近一次收到的完整请求文本 (含请求行/头/体; 试拉取测试据此断言构造结果)
    std::string LastRequest() const {
        std::lock_guard<std::mutex> lock(_reqMtx);
        return _lastRequest;
    }

private:
    static void AppendChunk(std::ostringstream& os, const std::string& part) {
        os << std::hex << part.size() << "\r\n" << part << "\r\n";
    }

    void Serve() {
        // 非阻塞 accept: close() 无法唤醒阻塞中的同步 accept (Linux 尤其明显),
        // 会让 Stop() 的 join 永久挂起。轮询 would_block 让停止可控。
        asio::error_code nbec;
        _acceptor.non_blocking(true, nbec);
        while (!_stopping.load()) {
            std::shared_ptr<asio::ip::tcp::socket> sock(
                new asio::ip::tcp::socket(_io));
            asio::error_code ec;
            _acceptor.accept(*sock, ec);
            if (ec == asio::error::would_block || ec == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (ec) return;

            // accept 出来的句柄在 Linux 上不继承 O_NONBLOCK, 但显式归位更安全。
            asio::error_code bec;
            sock->non_blocking(false, bec);
            _connections.fetch_add(1);

            ServeConnection(*sock);
        }
    }

    void ServeConnection(asio::ip::tcp::socket& sock) {
        asio::streambuf request;
        int served = 0;
        while (!_stopping.load()) {
            asio::error_code ec;
            asio::read_until(sock, request, "\r\n\r\n", ec);
            if (ec) return;                 // 对端关闭 / 服务端停止
            {
                std::lock_guard<std::mutex> lock(_reqMtx);
                _lastRequest.assign(asio::buffers_begin(request.data()),
                                    asio::buffers_end(request.data()));
            }
            request.consume(request.size());

            std::ostringstream resp;
            resp << "HTTP/1.1 " << _status.load() << " X\r\n"
                 << "Content-Type: application/json\r\n";
            if (!_keepAlive.load()) resp << "Connection: close\r\n";
            if (_chunked.load()) {
                resp << "Transfer-Encoding: chunked\r\n\r\n";
                const std::string body = _body;
                if (_badChunkSize.load()) {
                    resp << "ZZ\r\n" << body << "\r\n0\r\n\r\n";
                } else if (body.empty()) {
                    resp << "0\r\n\r\n";
                } else {
                    const std::size_t first = body.size() / 2;
                    AppendChunk(resp, body.substr(0, first));
                    AppendChunk(resp, body.substr(first));
                    // 最后一个 0 长度块之后才是 trailer 行 (RFC 7230 §4.1)。
                    if (_trailers.load()) {
                        resp << "0\r\nX-Trace: chunked-test\r\n\r\n";
                    } else {
                        resp << "0\r\n\r\n";
                    }
                }
            } else {
                resp << "Content-Length: " << _body.size() << "\r\n\r\n" << _body;
            }
            const std::string text = resp.str();
            asio::write(sock, asio::buffer(text), ec);
            _hits.fetch_add(1);
            if (ec) return;
            ++served;

            const int closeAfter = _closeAfter.load();
            if (!_keepAlive.load() || (closeAfter > 0 && served >= closeAfter)) {
                asio::error_code ignored;
                sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                sock.close(ignored);
                return;
            }
        }
    }

    asio::io_context _io;
    asio::ip::tcp::acceptor _acceptor;
    std::thread _thread;
    std::atomic<bool> _stopping;
    std::atomic<int> _status;
    std::atomic<int> _hits;
    std::atomic<int> _connections;
    std::atomic<bool> _keepAlive;
    std::atomic<int> _closeAfter;
    std::atomic<bool> _chunked;
    std::atomic<bool> _trailers;
    std::atomic<bool> _badChunkSize;
    std::string _body;
    mutable std::mutex _reqMtx;
    std::string _lastRequest;
};

HttpPullConfig MakePullConfig(unsigned short port) {
    HttpPullConfig cfg;
    std::ostringstream url;
    url << "http://127.0.0.1:" << port << "/api/values";
    cfg.url = url.str();
    cfg.method = "GET";
    cfg.intervalMs = 100;
    cfg.timeoutMs = 2000;
    cfg.tagField = "tag";
    cfg.valueField = "value";
    cfg.tsField = "ts";
    cfg.arrayOfItems = true;
    return cfg;
}

} // namespace

TEST(HttpPullTest, ParseUrlAcceptsPlainHttp) {
    std::string host, path, err;
    uint16_t port = 0;

    EXPECT_TRUE(HttpPuller::ParseUrl("http://127.0.0.1:8080/api/values",
                                     host, port, path, err));
    EXPECT_EQ(host, std::string("127.0.0.1"));
    EXPECT_EQ(static_cast<int>(port), 8080);
    EXPECT_EQ(path, std::string("/api/values"));

    EXPECT_TRUE(HttpPuller::ParseUrl("http://plc.local", host, port, path, err));
    EXPECT_EQ(host, std::string("plc.local"));
    EXPECT_EQ(static_cast<int>(port), 80);
    EXPECT_EQ(path, std::string("/"));

    EXPECT_TRUE(HttpPuller::ParseUrl("http://[::1]:9000/x?a=1", host, port, path, err));
    EXPECT_EQ(host, std::string("::1"));
    EXPECT_EQ(static_cast<int>(port), 9000);
    EXPECT_EQ(path, std::string("/x?a=1"));
}

TEST(HttpPullTest, ParseUrlRejectsUnsupportedForms) {
    std::string host, path, err;
    uint16_t port = 0;

    EXPECT_FALSE(HttpPuller::ParseUrl("https://127.0.0.1/x", host, port, path, err));
    EXPECT_FALSE(HttpPuller::ParseUrl("ftp://127.0.0.1/x", host, port, path, err));
    EXPECT_FALSE(HttpPuller::ParseUrl("http://", host, port, path, err));
    EXPECT_FALSE(HttpPuller::ParseUrl("http://user:pw@host/x", host, port, path, err));
    EXPECT_FALSE(HttpPuller::ParseUrl("http://host:70000/x", host, port, path, err));
    EXPECT_FALSE(HttpPuller::ParseUrl("http://host:abc/x", host, port, path, err));
}

TEST(HttpPullTest, ArrayItemsMapEveryElement) {
    const std::string body =
        "[{\"tag\":\"A\",\"value\":1,\"ts\":1700000000},"
        " {\"tag\":\"B\",\"value\":true}]";
    std::vector<MappingItem> items;
    std::string err;

    EXPECT_TRUE(PayloadMapper::MapJson(body, "tag", "value", "ts", "", items, err, true));
    ASSERT_EQ(items.size(), static_cast<std::size_t>(2));
    EXPECT_EQ(items[0].tagName, std::string("A"));
    EXPECT_EQ(items[0].timestampMs, static_cast<int64_t>(1700000000000LL));
    EXPECT_EQ(items[1].tagName, std::string("B"));
    EXPECT_EQ(items[1].timestampMs, static_cast<int64_t>(0));
}

TEST(HttpPullTest, ArrayItemsRejectMalformedElements) {
    std::vector<MappingItem> items;
    std::string err;

    // 缺 valueField
    EXPECT_FALSE(PayloadMapper::MapJson("[{\"tag\":\"A\"}]", "tag", "value", "ts",
                                        "", items, err, true));
    EXPECT_FALSE(err.empty());

    // 元素不是对象
    EXPECT_FALSE(PayloadMapper::MapJson("[1,2]", "tag", "value", "ts",
                                        "", items, err, true));

    // 空数组
    EXPECT_FALSE(PayloadMapper::MapJson("[]", "tag", "value", "ts",
                                        "", items, err, true));

    // arrayAsItems=false 时保持旧语义: 整个数组是一个值 (此处无 defaultTag 故失败)
    EXPECT_FALSE(PayloadMapper::MapJson("[{\"tag\":\"A\",\"value\":1}]", "tag", "value",
                                        "ts", "", items, err, false));
}

TEST(HttpPullTest, PullerDeliversFrame) {
    FakeHttpServer server;
    server.SetResponse(200, "[{\"tag\":\"T\",\"value\":7}]");
    server.Start();

    asio::io_context io;
    // 没有 work guard 时 io.run() 会在没有 pending handler 时立即返回,
    // 而首帧要等工作线程完成 HTTP 交换后才 post 进来。
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    std::shared_ptr<HttpPuller> puller = std::make_shared<HttpPuller>(
        io, "D-HTTP", MakePullConfig(server.Port()));

    std::mutex mtx;
    std::string payload;
    puller->Start(
        [&mtx, &payload](const InboundFrame& f) {
            std::lock_guard<std::mutex> lock(mtx);
            payload = f.payload;
        },
        [](const std::string&) {});

    std::thread ioThread([&io]() { io.run(); });
    EXPECT_TRUE(WaitFor([&mtx, &payload]() {
        std::lock_guard<std::mutex> lock(mtx);
        return !payload.empty();
    }, 3000));

    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(payload, std::string("[{\"tag\":\"T\",\"value\":7}]"));
    }

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

TEST(HttpPullTest, ManagerAppliesPulledValues) {
    FakeHttpServer server;
    server.SetResponse(200,
        "[{\"tag\":\"P.Temp\",\"value\":23.5},{\"tag\":\"P.Run\",\"value\":true}]");
    server.Start();

    MyProt::Core::DeviceConfig device;
    device.id = "P";
    device.connection.host = "127.0.0.1";

    MyProt::Core::PullConfig pull;
    pull.type = "http";
    pull.http = MakePullConfig(server.Port());
    device.pull = MyProt::Core::Optional<MyProt::Core::PullConfig>(pull);

    MyProt::Core::TagDefinition temp;
    temp.name = "P.Temp";
    temp.deviceId = "P";
    temp.source = "pull";
    temp.finalType = "Double";

    MyProt::Core::TagDefinition run;
    run.name = "P.Run";
    run.deviceId = "P";
    run.source = "pull";
    run.finalType = "Bool";

    std::vector<MyProt::Core::DeviceConfig> devices(1, device);
    std::vector<MyProt::Core::TagDefinition> tags;
    tags.push_back(temp);
    tags.push_back(run);

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    manager.Apply(devices, tags);
    EXPECT_TRUE(manager.IsRunning());
    EXPECT_EQ(manager.DeviceCount(), static_cast<std::size_t>(1));

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("P.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Good
            && value.typedValue.type == MyProt::Core::ValueType::Double
            && value.typedValue.d == 23.5;
    }, 3000));

    MyProt::Core::TagValue runValue;
    EXPECT_TRUE(store.Lookup("P.Run", runValue));
    EXPECT_TRUE(runValue.typedValue.type == MyProt::Core::ValueType::Bool);
    EXPECT_TRUE(runValue.typedValue.b);

    manager.Stop();
    server.Stop();
}

TEST(HttpPullTest, HttpErrorMarksTagsBad) {
    FakeHttpServer server;
    server.SetResponse(500, "upstream exploded");
    server.Start();

    MyProt::Core::DeviceConfig device;
    device.id = "E";
    device.connection.host = "127.0.0.1";

    MyProt::Core::PullConfig pull;
    pull.type = "http";
    pull.http = MakePullConfig(server.Port());
    device.pull = MyProt::Core::Optional<MyProt::Core::PullConfig>(pull);

    MyProt::Core::TagDefinition tag;
    tag.name = "E.Temp";
    tag.deviceId = "E";
    tag.source = "pull";
    tag.finalType = "Double";

    std::vector<MyProt::Core::DeviceConfig> devices(1, device);
    std::vector<MyProt::Core::TagDefinition> tags(1, tag);

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    manager.Apply(devices, tags);

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("E.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Bad;
    }, 3000));

    manager.Stop();
    server.Stop();
}

// 回归: pull 明确失败时质量只由 Bad 表达, 静默看门狗不得在同一段故障里补发
// Uncertain (否则会出现 Bad→Uncertain→Bad 抖动).
TEST(HttpPullTest, FailedPullKeepsBadWithoutUncertainFlip) {
    FakeHttpServer server;
    server.SetResponse(500, "upstream down");
    server.Start();

    MyProt::Core::DeviceConfig device;
    device.id = "F";
    device.connection.host = "127.0.0.1";

    MyProt::Core::PullConfig pull;
    pull.type = "http";
    pull.http = MakePullConfig(server.Port());
    pull.http.maxSilenceMs = 300;   // 远小于看门狗观测窗口, 逼出静默分支
    device.pull = MyProt::Core::Optional<MyProt::Core::PullConfig>(pull);

    MyProt::Core::TagDefinition tag;
    tag.name = "F.Temp";
    tag.deviceId = "F";
    tag.source = "pull";
    tag.finalType = "Double";

    std::vector<MyProt::Core::DeviceConfig> devices(1, device);
    std::vector<MyProt::Core::TagDefinition> tags(1, tag);

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    manager.Apply(devices, tags);

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("F.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Bad;
    }, 3000));

    // 持续失败期间逐点采样: 不允许出现任何一次 Uncertain
    bool sawUncertain = false;
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        MyProt::Core::TagValue value;
        if (store.Lookup("F.Temp", value)
                && value.quality == MyProt::Core::QualityCode::Uncertain) {
            sawUncertain = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_FALSE(sawUncertain);

    MyProt::Core::TagValue finalValue;
    EXPECT_TRUE(store.Lookup("F.Temp", finalValue));
    EXPECT_TRUE(finalValue.quality == MyProt::Core::QualityCode::Bad);

    manager.Stop();
    server.Stop();
}

// 回归: HTTP 持续成功但响应里没有任何本设备标签 → 属于"静默",
// maxSilenceMs 后应报 Uncertain 且保留上次 Good 值 (ADR-0023 §2.5).
TEST(HttpPullTest, PullWithoutMatchingTagsTurnsUncertain) {
    FakeHttpServer server;
    server.SetResponse(200, "[{\"tag\":\"U.Temp\",\"value\":42}]");
    server.Start();

    MyProt::Core::DeviceConfig device;
    device.id = "U";
    device.connection.host = "127.0.0.1";

    MyProt::Core::PullConfig pull;
    pull.type = "http";
    pull.http = MakePullConfig(server.Port());
    pull.http.maxSilenceMs = 300;
    device.pull = MyProt::Core::Optional<MyProt::Core::PullConfig>(pull);

    MyProt::Core::TagDefinition tag;
    tag.name = "U.Temp";
    tag.deviceId = "U";
    tag.source = "pull";
    tag.finalType = "Double";

    std::vector<MyProt::Core::DeviceConfig> devices(1, device);
    std::vector<MyProt::Core::TagDefinition> tags(1, tag);

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    manager.Apply(devices, tags);

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("U.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Good
            && value.typedValue.type == MyProt::Core::ValueType::Double
            && value.typedValue.d == 42.0;
    }, 3000));

    // 之后只回无关标签: 请求成功, 但没有一条命中 U.Temp
    server.SetResponse(200, "[{\"tag\":\"U.Other\",\"value\":1}]");

    EXPECT_TRUE(WaitFor([&store]() {
        MyProt::Core::TagValue value;
        return store.Lookup("U.Temp", value)
            && value.quality == MyProt::Core::QualityCode::Uncertain;
    }, 3000));

    // Uncertain 保留上次 Good 值, 不把值清空
    MyProt::Core::TagValue kept;
    EXPECT_TRUE(store.Lookup("U.Temp", kept));
    EXPECT_TRUE(kept.typedValue.type == MyProt::Core::ValueType::Double);
    EXPECT_TRUE(kept.typedValue.d == 42.0);

    manager.Stop();
    server.Stop();
}

// 退避策略是纯函数: 连续失败次数 -> 下一轮等待时长, 便于确定性断言。
TEST(HttpPullTest, RetryDelayGrowsExponentially) {
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(0, 1000, 1000, 30000), 1000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(1, 1000, 1000, 30000), 1000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(2, 1000, 1000, 30000), 2000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(3, 1000, 1000, 30000), 4000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(4, 1000, 1000, 30000), 8000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(5, 1000, 1000, 30000), 16000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(6, 1000, 1000, 30000), 30000);
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(60, 1000, 1000, 30000), 30000);
    // backoffBaseMs=0 关闭退避: 始终按 intervalMs
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(5, 1000, 0, 30000), 1000);
    // 退避不得短于采集间隔
    EXPECT_EQ(HttpPuller::ComputeRetryDelayMs(3, 5000, 1000, 30000), 5000);
}

// keep-alive: 服务端允许复用时, 多轮拉取只应建一条 TCP 连接。
TEST(HttpPullTest, ReusesConnectionWhenServerAllowsKeepAlive) {
    FakeHttpServer server;
    server.SetKeepAlive(true);
    server.SetResponse(200, "[{\"tag\":\"K\",\"value\":1}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    cfg.keepAlive = true;
    std::shared_ptr<HttpPuller> puller = std::make_shared<HttpPuller>(io, "D-KA", cfg);

    std::atomic<int> frames(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [](const std::string&) {});
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&server]() { return server.Hits() >= 4; }, 5000));
    EXPECT_TRUE(frames.load() >= 3);
    EXPECT_EQ(server.Connections(), 1);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// 服务端回 Connection: close 时必须每轮重连, 不能误复用已关闭的连接。
TEST(HttpPullTest, ReconnectsWhenServerAsksToClose) {
    FakeHttpServer server;   // 默认 keepAlive=false -> 每轮 Connection: close
    server.SetResponse(200, "[{\"tag\":\"C\",\"value\":2}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    cfg.keepAlive = true;    // 客户端想复用, 但服务端要求关闭
    std::shared_ptr<HttpPuller> puller = std::make_shared<HttpPuller>(io, "D-CC", cfg);

    std::atomic<int> frames(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [](const std::string&) {});
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&server]() { return server.Hits() >= 3; }, 5000));
    EXPECT_TRUE(frames.load() >= 3);
    EXPECT_TRUE(server.Connections() >= 3);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// 陈旧 keep-alive 连接 (对端已静默关闭) 应自动换新连接重试, 不得上报失败。
TEST(HttpPullTest, RetriesOnceOnStaleKeepAliveConnection) {
    FakeHttpServer server;
    server.SetKeepAlive(true);
    server.SetCloseAfter(2);          // 每条连接服务 2 轮后静默关闭
    server.SetResponse(200, "[{\"tag\":\"S\",\"value\":3}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    cfg.keepAlive = true;
    std::shared_ptr<HttpPuller> puller = std::make_shared<HttpPuller>(io, "D-ST", cfg);

    std::atomic<int> frames(0);
    std::atomic<int> errors(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [&errors](const std::string&) { errors.fetch_add(1); });
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&server]() { return server.Hits() >= 6; }, 8000));
    EXPECT_TRUE(frames.load() >= 5);
    EXPECT_EQ(errors.load(), 0);
    EXPECT_TRUE(server.Connections() >= 2);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// chunked 响应: 解码后的 body 应与原始 JSON 完全一致。
TEST(HttpPullTest, DecodesChunkedResponse) {
    const std::string body =
        "[{\"tag\":\"CH\",\"value\":11},{\"tag\":\"CH2\",\"value\":12}]";
    FakeHttpServer server;
    server.SetChunked(true);          // body 被拆成两块发送
    server.SetResponse(200, body);
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 50;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "D-CH", cfg);

    std::mutex mtx;
    std::string payload;
    puller->Start(
        [&mtx, &payload](const InboundFrame& f) {
            std::lock_guard<std::mutex> lock(mtx);
            payload = f.payload;
        },
        [](const std::string&) {});
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&mtx, &payload]() {
        std::lock_guard<std::mutex> lock(mtx);
        return !payload.empty();
    }, 3000));
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_EQ(payload, body);
    }

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// chunked 也是自定界的响应: 服务端允许 keep-alive 时不得因为"无 Content-Length"而重连。
TEST(HttpPullTest, ChunkedResponseKeepsConnectionAlive) {
    FakeHttpServer server;
    server.SetKeepAlive(true);
    server.SetChunked(true);
    server.SetResponse(200, "[{\"tag\":\"CK\",\"value\":1}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    cfg.keepAlive = true;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "D-CKA", cfg);

    std::atomic<int> frames(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [](const std::string&) {});
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&server]() { return server.Hits() >= 4; }, 5000));
    EXPECT_TRUE(frames.load() >= 3);
    EXPECT_EQ(server.Connections(), 1);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// 结束块之后的 trailer 行必须被吃掉, 不能污染下一轮响应。
TEST(HttpPullTest, ChunkedTrailersAreAccepted) {
    const std::string body = "[{\"tag\":\"CT\",\"value\":9}]";
    FakeHttpServer server;
    server.SetKeepAlive(true);
    server.SetChunked(true, true /*trailers*/);
    server.SetResponse(200, body);
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    cfg.keepAlive = true;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "D-CT", cfg);

    std::atomic<int> frames(0);
    std::atomic<int> errors(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [&errors](const std::string&) { errors.fetch_add(1); });
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&frames]() { return frames.load() >= 3; }, 5000));
    EXPECT_EQ(errors.load(), 0);
    EXPECT_EQ(server.Connections(), 1);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// 非法 chunk 长度行 -> 明确报错 (不得静默当成 EOF 或部分 body)。
TEST(HttpPullTest, MalformedChunkSizeReportsError) {
    FakeHttpServer server;
    server.SetChunked(true, false, true /*badChunkSize*/);
    server.SetResponse(200, "[{\"tag\":\"CB\",\"value\":1}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 50;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "D-CB", cfg);

    std::mutex mtx;
    std::string lastError;
    puller->Start([](const InboundFrame&) {},
                  [&mtx, &lastError](const std::string& message) {
                      std::lock_guard<std::mutex> lock(mtx);
                      lastError = message;
                  });
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&mtx, &lastError]() {
        std::lock_guard<std::mutex> lock(mtx);
        return !lastError.empty();
    }, 3000));
    {
        std::lock_guard<std::mutex> lock(mtx);
        EXPECT_TRUE(lastError.find("chunk") != std::string::npos);
    }

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// B3: 成功轮次必须产出 rounds / last-success / connections 指标。
TEST(HttpPullTest, ExposesPullMetrics) {
    FakeHttpServer server;
    server.SetChunked(true);
    server.SetResponse(200, "[{\"tag\":\"M\",\"value\":1}]");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 30;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "M-PULL", cfg);

    std::atomic<int> frames(0);
    puller->Start([&frames](const InboundFrame&) { frames.fetch_add(1); },
                  [](const std::string&) {});
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&frames]() { return frames.load() >= 2; }, 5000));

    const std::string text =
        MyProt::Core::MetricsRegistry::Instance().RenderPrometheus();
    EXPECT_TRUE(text.find("myprot_pull_rounds_total{device=\"M-PULL\"}")
                != std::string::npos);
    EXPECT_TRUE(text.find("myprot_pull_last_success_timestamp_seconds{device=\"M-PULL\"}")
                != std::string::npos);
    EXPECT_TRUE(text.find("myprot_pull_connections_total{device=\"M-PULL\"}")
                != std::string::npos);
    EXPECT_TRUE(text.find("myprot_pull_consecutive_failures{device=\"M-PULL\"}")
                != std::string::npos);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// B3: 失败轮次产出 failures / 连续失败 / 退避指标。
TEST(HttpPullTest, ExposesPullFailureMetrics) {
    FakeHttpServer server;
    server.SetResponse(500, "down");
    server.Start();

    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work =
        asio::make_work_guard(io);
    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.intervalMs = 50;
    cfg.backoffBaseMs = 20;          // 测试窗口内快速累积失败
    cfg.backoffMaxMs = 100;
    std::shared_ptr<HttpPuller> puller =
        std::make_shared<HttpPuller>(io, "M-FAIL", cfg);

    std::atomic<int> errors(0);
    puller->Start([](const InboundFrame&) {},
                  [&errors](const std::string&) { errors.fetch_add(1); });
    std::thread ioThread([&io]() { io.run(); });

    EXPECT_TRUE(WaitFor([&errors]() { return errors.load() >= 2; }, 5000));

    const std::string text =
        MyProt::Core::MetricsRegistry::Instance().RenderPrometheus();
    EXPECT_TRUE(text.find("myprot_pull_failures_total{device=\"M-FAIL\"}")
                != std::string::npos);
    EXPECT_TRUE(text.find("myprot_pull_consecutive_failures{device=\"M-FAIL\"}")
                != std::string::npos);
    EXPECT_TRUE(text.find("myprot_pull_backoff_ms{device=\"M-FAIL\"}")
                != std::string::npos);

    puller->Stop();
    work.reset();
    io.stop();
    if (ioThread.joinable()) ioThread.join();
    server.Stop();
}

// B4: manager 把拉取可达性上报给 App 安装的 health handler (设备生命周期入口)。
TEST(HttpPullTest, ManagerReportsPullHealth) {
    FakeHttpServer server;
    server.SetResponse(200, "[{\"tag\":\"H.Temp\",\"value\":1}]");
    server.Start();

    MyProt::Core::DeviceConfig device;
    device.id = "H";
    device.connection.host = "127.0.0.1";
    MyProt::Core::PullConfig pull;
    pull.type = "http";
    pull.http = MakePullConfig(server.Port());
    pull.http.intervalMs = 50;
    device.pull = MyProt::Core::Optional<MyProt::Core::PullConfig>(pull);

    MyProt::Core::TagDefinition tag;
    tag.name = "H.Temp";
    tag.deviceId = "H";
    tag.source = "pull";
    tag.finalType = "Double";

    MyProt::Polling::LatestValueStore store;
    SouthboundManager manager(store);
    std::mutex mtx;
    bool sawHealthy = false;
    bool sawUnhealthy = false;
    manager.SetHealthHandler(
        [&mtx, &sawHealthy, &sawUnhealthy](const std::string& deviceId, bool healthy) {
            if (deviceId != "H") return;
            std::lock_guard<std::mutex> lock(mtx);
            if (healthy) sawHealthy = true;
            else sawUnhealthy = true;
        });
    manager.Apply(std::vector<MyProt::Core::DeviceConfig>(1, device),
                  std::vector<MyProt::Core::TagDefinition>(1, tag));

    EXPECT_TRUE(WaitFor([&mtx, &sawHealthy]() {
        std::lock_guard<std::mutex> lock(mtx);
        return sawHealthy;
    }, 3000));

    // 服务端转为失败: 下一轮应上报 unhealthy
    server.SetResponse(500, "down");
    EXPECT_TRUE(WaitFor([&mtx, &sawUnhealthy]() {
        std::lock_guard<std::mutex> lock(mtx);
        return sawUnhealthy;
    }, 5000));

    manager.Stop();
    server.Stop();
}

// ── Probe: 管理面「试拉取」— 一次性阻塞请求, 与调度循环共用请求构造 ──

TEST(HttpPullTest, ProbeReturnsStatusBodyAndElapsed) {
    FakeHttpServer server;
    server.SetResponse(200, "[{\"tag\":\"T\",\"value\":7}]");
    server.Start();

    const HttpPuller::ProbeResult r = HttpPuller::Probe(MakePullConfig(server.Port()));
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body, std::string("[{\"tag\":\"T\",\"value\":7}]"));
    EXPECT_TRUE(r.error.empty());
    EXPECT_TRUE(r.elapsedMs >= 0);
    EXPECT_EQ(server.Hits(), 1);
    server.Stop();
}

TEST(HttpPullTest, ProbeReportsHttpErrorAsFailure) {
    FakeHttpServer server;
    server.SetResponse(500, "boom");
    server.Start();

    const HttpPuller::ProbeResult r = HttpPuller::Probe(MakePullConfig(server.Port()));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 500);
    EXPECT_TRUE(r.body.empty());
    EXPECT_FALSE(r.error.empty());
    // 错误文本带上状态码与响应片段, 前端可直接展示
    EXPECT_TRUE(r.error.find("500") != std::string::npos);
    server.Stop();
}

TEST(HttpPullTest, ProbeOpensFreshConnectionEachTime) {
    FakeHttpServer server;
    server.SetResponse(200, "{}");
    server.SetKeepAlive(true);          // 服务端允许复用
    server.Start();

    const HttpPullConfig cfg = MakePullConfig(server.Port());
    EXPECT_TRUE(HttpPuller::Probe(cfg).ok);
    EXPECT_TRUE(HttpPuller::Probe(cfg).ok);
    // Probe 固定 keepAlive=false: 每次新建连接, 不碰拉取线程的 keep-alive 连接
    EXPECT_EQ(server.Connections(), 2);
    server.Stop();
}

TEST(HttpPullTest, ProbeBuildsSameRequestAsPullLoop) {
    FakeHttpServer server;
    server.SetResponse(200, "{}");
    server.Start();

    HttpPullConfig cfg = MakePullConfig(server.Port());
    cfg.method = "POST";
    cfg.body = "{\"q\":1}";
    cfg.headers["X-Token"] = "abc";
    cfg.headers["Host"] = "evil";                  // 受控头: 必须被忽略
    cfg.headers["Content-Type"] = "text/plain";    // 用户显式声明: 保留

    EXPECT_TRUE(HttpPuller::Probe(cfg).ok);
    const std::string req = server.LastRequest();
    EXPECT_TRUE(req.find("POST /api/values HTTP/1.1\r\n") == 0);
    EXPECT_TRUE(req.find("Host: evil") == std::string::npos);
    EXPECT_TRUE(req.find("X-Token: abc\r\n") != std::string::npos);
    EXPECT_TRUE(req.find("Content-Type: text/plain\r\n") != std::string::npos);
    EXPECT_TRUE(req.find("Content-Length: 7\r\n") != std::string::npos);
    EXPECT_TRUE(req.find("Connection: close\r\n") != std::string::npos);
    EXPECT_TRUE(req.find("{\"q\":1}") != std::string::npos);
    server.Stop();
}

TEST(HttpPullTest, ProbeReportsUnreachableEndpoint) {
    HttpPullConfig cfg;
    cfg.url = "http://127.0.0.1:1/api";        // 未监听端口 → 立即拒连
    cfg.method = "GET";
    cfg.timeoutMs = 1000;

    const HttpPuller::ProbeResult r = HttpPuller::Probe(cfg);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.elapsedMs >= 0);
}

TEST(HttpPullTest, ProbeRejectsInvalidUrl) {
    HttpPullConfig cfg;
    cfg.url = "https://127.0.0.1/x";          // v1 不支持 TLS

    const HttpPuller::ProbeResult r = HttpPuller::Probe(cfg);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.status, 0);
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.body.empty());
}

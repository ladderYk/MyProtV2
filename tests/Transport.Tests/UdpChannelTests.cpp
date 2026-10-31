// tests/Transport.Tests/UdpChannelTests.cpp — UDP 数据报通道单元测试

#include "MyProt/Transport/UdpChannel.hpp"
#include "MiniTest.hpp"

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using MyProt::Transport::UdpChannel;
using Bytes = std::vector<std::uint8_t>;

/// 起一个回显服务端: 收到一个数据报就原样发回, 然后不再挂起 (让 io.run 自然结束)
std::shared_ptr<asio::ip::udp::socket> StartUdpEcho(asio::io_context& io) {
    std::shared_ptr<asio::ip::udp::socket> sock =
        std::make_shared<asio::ip::udp::socket>(
            io, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    std::shared_ptr<Bytes> buf = std::make_shared<Bytes>(256);
    std::shared_ptr<asio::ip::udp::endpoint> sender =
        std::make_shared<asio::ip::udp::endpoint>();
    sock->async_receive_from(asio::buffer(*buf), *sender,
        [sock, buf, sender](const asio::error_code& ec, std::size_t n) {
            if (ec) return;
            asio::error_code ignore;
            sock->send_to(asio::buffer(*buf, n), *sender, 0, ignore);
        });
    return sock;
}

/// 起一个只收不回的服务端 (模拟设备不应答 → 通道按超时失败)
std::shared_ptr<asio::ip::udp::socket> StartUdpSilent(
        asio::io_context& io, Bytes& received) {
    std::shared_ptr<asio::ip::udp::socket> sock =
        std::make_shared<asio::ip::udp::socket>(
            io, asio::ip::udp::endpoint(asio::ip::udp::v4(), 0));
    std::shared_ptr<Bytes> buf = std::make_shared<Bytes>(256);
    std::shared_ptr<asio::ip::udp::endpoint> sender =
        std::make_shared<asio::ip::udp::endpoint>();
    // sender 必须随 operation 一起存活: async_receive_from 以引用保存对端地址,
    // 完成时才写入。此前 lambda 漏捕 sender, helper 返回后该 endpoint 即被释放,
    // 回调写入已释放内存 → 堆损坏 → 进程偶发在 WS2_32 内 0xC0000005。
    sock->async_receive_from(asio::buffer(*buf), *sender,
        [&received, buf, sender](const asio::error_code& ec, std::size_t n) {
            if (ec) return;
            received.assign(buf->begin(), buf->begin() + n);
            // 故意不回: 通道端 ReceiveOne 永无应答, 看门狗到期判 Timeout
        });
    return sock;
}

} // namespace

// 一个数据报 = 一帧: 发出请求, 收到回显即作为完整响应返回, 不做流内拼接。
TEST(UdpChannelTest, SendReceiveEchoesOneDatagram) {
    asio::io_context io;
    std::shared_ptr<asio::ip::udp::socket> echo = StartUdpEcho(io);
    const unsigned short port = echo->local_endpoint().port();

    std::shared_ptr<UdpChannel> channel =
        std::make_shared<UdpChannel>(io, "test-udp-echo");
    MyProt::Core::ConnectionConfig endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;

    bool connected = false;
    bool gotResponse = false;
    Bytes response;
    std::string err;
    channel->Connect(endpoint, std::chrono::milliseconds(1000),
        [&, channel](MyProt::Core::Expected<void> cr) {
            connected = cr.has_value();
            if (!cr.has_value()) return;
            channel->SendReceive(MyProt::Core::Bytes{0x01, 0x02, 0x03},
                                 nullptr,
                                 std::chrono::milliseconds(1000),
                [&](MyProt::Core::Expected<MyProt::Core::Bytes> rr) {
                    if (rr.has_value()) {
                        gotResponse = true;
                        response = std::move(rr.value());
                    } else {
                        err = rr.error().message;
                    }
                });
        });

    io.run();

    EXPECT_TRUE(connected);
    EXPECT_TRUE(gotResponse);
    EXPECT_TRUE(err.empty());
    EXPECT_EQ(response.size(), static_cast<std::size_t>(3));
    EXPECT_EQ(static_cast<int>(response[0]), 0x01);
    EXPECT_EQ(static_cast<int>(response[2]), 0x03);
    EXPECT_TRUE(channel->IsConnected());
}

// 无应答设备: 请求-应答按超时失败, 通道关闭 socket 并清连接标志 (下轮重建)。
TEST(UdpChannelTest, TimeoutsWhenPeerNeverReplies) {
    asio::io_context io;
    Bytes received;
    std::shared_ptr<asio::ip::udp::socket> silent = StartUdpSilent(io, received);
    const unsigned short port = silent->local_endpoint().port();

    std::shared_ptr<UdpChannel> channel =
        std::make_shared<UdpChannel>(io, "test-udp-timeout");
    MyProt::Core::ConnectionConfig endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;

    MyProt::Core::Error::Code code = MyProt::Core::Error::Code::InternalError;
    bool endedDisconnected = false;
    channel->Connect(endpoint, std::chrono::milliseconds(1000),
        [&, channel](MyProt::Core::Expected<void> cr) {
            if (!cr.has_value()) return;
            channel->SendReceive(MyProt::Core::Bytes{0xAA, 0xBB}, nullptr,
                                 std::chrono::milliseconds(200),
                [&, channel](MyProt::Core::Expected<MyProt::Core::Bytes> rr) {
                    if (!rr.has_value()) code = rr.error().code;
                    endedDisconnected = !channel->IsConnected();
                });
        });

    io.run();

    EXPECT_TRUE(code == MyProt::Core::Error::Code::Timeout);
    EXPECT_TRUE(endedDisconnected);
    EXPECT_EQ(received.size(), static_cast<std::size_t>(2));
}

// 单在途守卫: 第一次 SendReceive 未终态时, 第二次立即 Busy 失败。
TEST(UdpChannelTest, SingleInFlightRejectsBusy) {
    asio::io_context io;
    Bytes received;
    std::shared_ptr<asio::ip::udp::socket> silent = StartUdpSilent(io, received);
    const unsigned short port = silent->local_endpoint().port();

    std::shared_ptr<UdpChannel> channel =
        std::make_shared<UdpChannel>(io, "test-udp-busy");
    MyProt::Core::ConnectionConfig endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;

    MyProt::Core::Error::Code secondCode = MyProt::Core::Error::Code::InternalError;
    channel->Connect(endpoint, std::chrono::milliseconds(1000),
        [&, channel](MyProt::Core::Expected<void> cr) {
            if (!cr.has_value()) return;
            // 第一次: 发出后挂起等应答 (silent 不回 → 稍后超时)
            channel->SendReceive(MyProt::Core::Bytes{0x01}, nullptr,
                                 std::chrono::milliseconds(200),
                [](MyProt::Core::Expected<MyProt::Core::Bytes>) {});
            // 第二次: 第一次仍在途 → 必须 Busy (同步返回)
            channel->SendReceive(MyProt::Core::Bytes{0x02}, nullptr,
                                 std::chrono::milliseconds(200),
                [&](MyProt::Core::Expected<MyProt::Core::Bytes> rr) {
                    if (!rr.has_value()) secondCode = rr.error().code;
                });
        });

    io.run();

    EXPECT_TRUE(secondCode == MyProt::Core::Error::Code::Busy);
}

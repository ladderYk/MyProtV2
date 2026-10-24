// tests/Transport.Tests/TcpChannelTests.cpp — TCP 通道单元测试

#include "MyProt/Transport/TcpChannel.hpp"
#include "MiniTest.hpp"

#include <asio.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

using MyProt::Transport::TcpChannel;

/// 起一个只接收不应答的 loopback 服务端 (模拟"设备不应答"的只发场景)
std::shared_ptr<asio::ip::tcp::acceptor> StartSilentServer(
    asio::io_context& io, std::vector<std::uint8_t>& received) {
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor =
        std::make_shared<asio::ip::tcp::acceptor>(
            io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));

    acceptor->async_accept(
        [&received](const asio::error_code& ec, asio::ip::tcp::socket sock) {
            if (ec) return;
            std::shared_ptr<asio::ip::tcp::socket> peer =
                std::make_shared<asio::ip::tcp::socket>(std::move(sock));
            std::shared_ptr<std::vector<std::uint8_t>> buf =
                std::make_shared<std::vector<std::uint8_t>>(64);
            peer->async_read_some(asio::buffer(*buf),
                [&received, buf, peer](const asio::error_code& rec,
                                       std::size_t n) {
                    if (!rec) {
                        received.assign(buf->begin(), buf->begin() + n);
                    }
                });
        });
    return acceptor;
}

} // namespace

// 只发: 设备不应答时 Send 在写完即完成, 不等待任何响应帧.
TEST(TcpChannelTest, OneWaySendCompletesWithoutResponse) {
    asio::io_context io;
    std::vector<std::uint8_t> received;
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor =
        StartSilentServer(io, received);
    const unsigned short port = acceptor->local_endpoint().port();

    std::shared_ptr<TcpChannel> channel =
        std::make_shared<TcpChannel>(io, "test-oneway");
    MyProt::Core::ConnectionConfig endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;

    bool connected = false;
    bool sent = false;
    std::string sendError;
    channel->Connect(endpoint, std::chrono::milliseconds(1000),
        [&, channel](MyProt::Core::Expected<void> cr) {
            connected = cr.has_value();
            if (!cr.has_value()) return;
            channel->Send(MyProt::Core::Bytes{0x01, 0x02, 0x03},
                          std::chrono::milliseconds(1000),
                [&](MyProt::Core::Expected<void> sr) {
                    sent = sr.has_value();
                    if (!sr.has_value()) sendError = sr.error().message;
                });
        });

    io.run();

    EXPECT_TRUE(connected);
    EXPECT_TRUE(sent);
    EXPECT_TRUE(sendError.empty());
    EXPECT_EQ(received.size(), static_cast<std::size_t>(3));
}

// 只发与请求-应答共用单在途守卫: 在途时再次 Send 立即 Busy.
TEST(TcpChannelTest, OneWaySendRejectsConcurrentRequest) {
    asio::io_context io;
    std::vector<std::uint8_t> received;
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor =
        StartSilentServer(io, received);
    const unsigned short port = acceptor->local_endpoint().port();

    std::shared_ptr<TcpChannel> channel =
        std::make_shared<TcpChannel>(io, "test-oneway-busy");
    MyProt::Core::ConnectionConfig endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = port;

    MyProt::Core::Error::Code secondCode =
        MyProt::Core::Error::Code::InternalError;
    channel->Connect(endpoint, std::chrono::milliseconds(1000),
        [&, channel](MyProt::Core::Expected<void> cr) {
            if (!cr.has_value()) return;
            channel->Send(MyProt::Core::Bytes{0xAA},
                          std::chrono::milliseconds(1000),
                [](MyProt::Core::Expected<void>) {});
            // 第一次 Send 仍在途 (挂起写/等 io 调度) → 第二次必须 Busy
            channel->Send(MyProt::Core::Bytes{0xBB},
                          std::chrono::milliseconds(1000),
                [&](MyProt::Core::Expected<void> sr) {
                    if (!sr.has_value()) secondCode = sr.error().code;
                });
        });

    io.run();

    EXPECT_TRUE(secondCode == MyProt::Core::Error::Code::Busy);
}

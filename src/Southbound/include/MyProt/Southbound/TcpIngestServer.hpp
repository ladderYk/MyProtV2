// src/Southbound/include/MyProt/Southbound/TcpIngestServer.hpp
// TCP listener for device-initiated push: the gateway is the server and devices connect in.
// v1 framing: LengthField / Fixed. Payload codec: JSON.

#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/FrameCodec.hpp"
#include "MyProt/Southbound/IInboundTransport.hpp"

namespace MyProt { namespace Southbound {

class TcpIngestServer : public IInboundTransport,
                        public std::enable_shared_from_this<TcpIngestServer> {
public:
    TcpIngestServer(asio::io_context& io,
                    const std::string& deviceId,
                    const Core::TcpIngestConfig& cfg);
    ~TcpIngestServer();

    void Start(FrameHandler onFrame, ErrorHandler onError);
    void Stop();
    bool IsRunning() const { return _running.load(); }

private:
    struct Connection {
        asio::ip::tcp::socket socket;
        std::vector<uint8_t> buffer;
        explicit Connection(asio::io_context& io) : socket(io) {}
    };

    void DoAccept();
    void ReadSome(const std::shared_ptr<Connection>& conn);
    bool ProcessBuffer(const std::shared_ptr<Connection>& conn);
    void CloseConnection(const std::shared_ptr<Connection>& conn, const std::string& reason);

    asio::io_context& _io;
    std::string _deviceId;
    Core::TcpIngestConfig _cfg;

    std::unique_ptr<asio::ip::tcp::acceptor> _acceptor;
    std::unique_ptr<Core::IFrameCodec> _codec;   // LengthField / Fixed -> opaque frames
    std::vector<std::shared_ptr<Connection> > _conns;

    FrameHandler _onFrame;
    ErrorHandler _onError;
    std::atomic<bool> _running;

    TcpIngestServer(const TcpIngestServer&) = delete;
    TcpIngestServer& operator=(const TcpIngestServer&) = delete;
};

}} // namespace MyProt::Southbound

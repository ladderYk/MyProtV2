// src/Southbound/src/TcpIngestServer.cpp

#include "MyProt/Southbound/TcpIngestServer.hpp"

#include <algorithm>
#include <chrono>

#include "MyProt/Core/Log.hpp"
#include "MyProt/Transport/StreamFrameCodec.hpp"

namespace MyProt { namespace Southbound {

namespace {

const std::size_t kReadChunk = 4096;

int64_t NowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

TcpIngestServer::TcpIngestServer(asio::io_context& io,
                                 const std::string& deviceId,
                                 const Core::TcpIngestConfig& cfg)
    : _io(io)
    , _deviceId(deviceId)
    , _cfg(cfg)
    , _running(false) {}

TcpIngestServer::~TcpIngestServer() {
    Stop();
}

void TcpIngestServer::Start(FrameHandler onFrame, ErrorHandler onError) {
    if (_running.load()) return;
    _onFrame = onFrame;
    _onError = onError;

    if (_cfg.framing.type == Core::FramingType::Message) {
        if (_onError) _onError("ingest.tcp.framing.type=Message 为 CAN 预留, v1 不支持");
        return;
    }
    if (_cfg.framing.type == Core::FramingType::Silence) {
        if (_onError) _onError("ingest.tcp.framing.type=Silence v1 未实现, 请改用 LengthField/Fixed");
        return;
    }
    if (_cfg.framing.type == Core::FramingType::Fixed
            && _cfg.framing.fixed.fixedLength <= 0) {
        if (_onError) _onError("ingest.tcp.framing.fixedLength 必须 > 0");
        return;
    }
    if (_cfg.framing.type != Core::FramingType::LengthField
            && _cfg.framing.type != Core::FramingType::Fixed) {
        if (_onError) _onError("ingest.tcp.framing.type 不受支持, 请使用 LengthField/Fixed");
        return;
    }
    _codec.reset(new Transport::StreamFrameCodec(_cfg.framing));

    asio::error_code ec;
    asio::ip::address addr;
    if (_cfg.bindAddress.empty() || _cfg.bindAddress == "0.0.0.0") {
        addr = asio::ip::address_v4::any();
    } else {
        addr = asio::ip::make_address(_cfg.bindAddress, ec);
        if (ec) {
            if (_onError) _onError("bindAddress 非法: " + _cfg.bindAddress);
            return;
        }
    }

    const asio::ip::tcp::endpoint endpoint(addr, _cfg.port);
    _acceptor.reset(new asio::ip::tcp::acceptor(_io));
    _acceptor->open(endpoint.protocol(), ec);
    if (!ec) _acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    if (!ec) _acceptor->bind(endpoint, ec);
    if (!ec) _acceptor->listen(asio::socket_base::max_connections, ec);
    if (ec) {
        if (_onError) {
            _onError("监听 " + _cfg.bindAddress + ":" + std::to_string(_cfg.port)
                     + " 失败: " + ec.message());
        }
        _acceptor.reset();
        return;
    }

    _running.store(true);
    DoAccept();
    LOG_INFO("Southbound", "TCP 监听已启动 device=%s %s:%u",
             _deviceId.c_str(),
             _cfg.bindAddress.empty() ? "0.0.0.0" : _cfg.bindAddress.c_str(),
             static_cast<unsigned>(_cfg.port));
}

void TcpIngestServer::Stop() {
    _running.store(false);
    if (_acceptor) {
        asio::error_code ec;
        _acceptor->close(ec);
        _acceptor.reset();
    }
    for (size_t i = 0; i < _conns.size(); ++i) {
        asio::error_code ec;
        _conns[i]->socket.close(ec);
    }
    _conns.clear();
    _onFrame = FrameHandler();
    _onError = ErrorHandler();
}

void TcpIngestServer::DoAccept() {
    if (!_running.load() || !_acceptor) return;
    std::shared_ptr<Connection> conn = std::make_shared<Connection>(_io);
    auto self = shared_from_this();
    _acceptor->async_accept(conn->socket,
        [self, conn](const asio::error_code& ec) {
            if (!self->_running.load()) return;
            if (!ec) {
                asio::error_code optEc;
                conn->socket.set_option(asio::ip::tcp::no_delay(true), optEc);
                self->_conns.push_back(conn);
                self->ReadSome(conn);
            }
            self->DoAccept();
        });
}

void TcpIngestServer::ReadSome(const std::shared_ptr<Connection>& conn) {
    if (!_running.load()) return;
    std::shared_ptr<std::vector<uint8_t> > chunk =
        std::make_shared<std::vector<uint8_t> >(kReadChunk);
    auto self = shared_from_this();
    conn->socket.async_read_some(asio::buffer(*chunk),
        [self, conn, chunk](const asio::error_code& ec, std::size_t n) {
            if (!self->_running.load()) return;
            if (ec) {
                self->CloseConnection(conn, ec.message());
                return;
            }
            conn->buffer.insert(conn->buffer.end(),
                                chunk->begin(),
                                chunk->begin() + static_cast<std::ptrdiff_t>(n));
            if (!self->ProcessBuffer(conn)) return;
            self->ReadSome(conn);
        });
}

bool TcpIngestServer::ProcessBuffer(const std::shared_ptr<Connection>& conn) {
    for (;;) {
        if (conn->buffer.empty()) return true;

        const Core::ByteView view(&conn->buffer[0], conn->buffer.size());
        Core::Frame frame;
        std::size_t consumed = 0;
        const Core::DecodeStatus status = _codec->Decode(view, consumed, frame);
        if (status == Core::DecodeStatus::NeedMore) return true;
        if (status == Core::DecodeStatus::Malformed) {
            CloseConnection(conn, "成帧失败");
            return false;
        }
        if (consumed == 0 || consumed > conn->buffer.size()) {
            CloseConnection(conn, "成帧器返回非法消费长度");
            return false;
        }
        conn->buffer.erase(conn->buffer.begin(),
                           conn->buffer.begin() + static_cast<std::ptrdiff_t>(consumed));

        if (_onFrame) {
            InboundFrame f;
            f.topic.clear();
            f.payload.assign(
                frame.payload.size > 0
                    ? reinterpret_cast<const char*>(frame.payload.data) : "",
                frame.payload.size);
            f.timestampMs = NowMs();
            _onFrame(f);
        }
    }
}

void TcpIngestServer::CloseConnection(const std::shared_ptr<Connection>& conn,
                                      const std::string& reason) {
    asio::error_code ec;
    conn->socket.close(ec);
    conn->buffer.clear();
    for (std::vector<std::shared_ptr<Connection> >::iterator it = _conns.begin();
            it != _conns.end(); ++it) {
        if (it->get() == conn.get()) { _conns.erase(it); break; }
    }
    if (!reason.empty()) {
        LOG_WARN("Southbound", "TCP 入站连接断开 device=%s: %s",
                 _deviceId.c_str(), reason.c_str());
    }
}

}} // namespace MyProt::Southbound

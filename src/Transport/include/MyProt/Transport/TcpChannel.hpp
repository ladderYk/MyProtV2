// src/Transport/include/MyProt/Transport/TcpChannel.hpp
// TCP 通道 — asio 异步回调模型 (C++11, ADR-0010 §2)

#pragma once
#include "IChannel.hpp"
#include <asio.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace MyProt { namespace Transport {

/// TCP 通道 — 基于 asio::ip::tcp::socket
/// 超时由 steady_timer 看门狗实现: 到期关闭套接字, 挂起操作以 operation_aborted 结束
class TcpChannel : public IChannel,
                   public std::enable_shared_from_this<TcpChannel> {
public:
    TcpChannel(asio::io_context& io, const std::string& channelId);
    virtual ~TcpChannel();

    void Connect(const Core::ConnectionConfig& endpoint,
                 std::chrono::milliseconds timeout,
                 ConnectHandler handler) override;

    void SendReceive(const Bytes& request,
                     std::shared_ptr<const Core::FramingConfig> framingConfig,
                     std::chrono::milliseconds timeout,
                     ReceiveHandler handler) override;

    void Disconnect() override;
    bool IsConnected() const override;
    Core::Error GetLastError() const override;

private:
    /// 收流操作状态 (回调链共享保活)
    struct RxOp {
        explicit RxOp(ReceiveHandler h)
            : handler(std::move(h)), done(false), maxFrameSize(1024) {}
        ReceiveHandler handler;
        Core::Bytes buffer;
        std::shared_ptr<const Core::FramingConfig> framing;
        std::shared_ptr<asio::steady_timer> watchdog;
        size_t maxFrameSize;
        bool done;
    };

    /// 追加读取一块数据; 每次到达后按成帧规则判定完整性
    void ReadChunk(const std::shared_ptr<RxOp>& op);

    /// 记录最近错误 (线程安全)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    std::string _channelId;
    std::unique_ptr<asio::ip::tcp::socket> _socket;
    asio::ip::tcp::resolver _resolver;
    std::atomic<bool> _connected;
    // 单通道单请求守卫: 同一时刻仅允许一笔在途 SendReceive
    // (socket 不支持并发读写; 第二笔立即 Busy 快速失败)
    std::atomic<bool> _inFlight;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;

    // 注: _lastEndpoint / _lastConnectTimeout 已撤回 (2026-08-31)。
    // 原 SendReceive 内部"借一次重发"会与 ChannelManager 端 GetOrCreateChannel
        // 形成双重 Connect 风暴; 重连职责在 ChannelManager, 本类不缓存

    // 禁止拷贝
    TcpChannel(const TcpChannel&) = delete;
    TcpChannel& operator=(const TcpChannel&) = delete;
};

}} // namespace MyProt::Transport

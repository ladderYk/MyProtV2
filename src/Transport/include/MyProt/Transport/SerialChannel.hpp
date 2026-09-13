// src/Transport/include/MyProt/Transport/SerialChannel.hpp
// 串口通道 — 异步回调模型 + 静默成帧 (C++11, ADR-0010 §2)

#pragma once
#include <memory>
#include <string>
#include <mutex>
#include <vector>
#include <atomic>
#include <asio.hpp>
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Transport {

/// 串口通道 — 基于 asio::serial_port
/// 静默成帧: 字符间隔超过 frameGap 即判定一帧结束 (Modbus RTU 3.5 字符约定)
class SerialChannel : public IChannel,
                      public std::enable_shared_from_this<SerialChannel> {
public:
    SerialChannel(asio::io_context& io,
                  const Core::SerialTransportConfig& serialConfig,
                  const std::string& channelId);

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
    /// 静默成帧收流操作状态 (回调链共享保活)
    struct OpState;

    /// 追加读取一块字符; 每次到达重启静默定时器
    void ReadSome(const std::shared_ptr<OpState>& op,
                  const std::shared_ptr<asio::serial_port>& port);

    /// 记录最近错误 (线程安全)
    void SetError(Core::Error::Code code, const std::string& msg);

    asio::io_context& _io;
    Core::SerialTransportConfig _serial;
    std::string _channelId;
    std::shared_ptr<asio::serial_port> _port;
    std::atomic<bool> _connected;
    mutable std::mutex _errorMutex;
    Core::Error _lastError;

    // 禁止拷贝
    SerialChannel(const SerialChannel&) = delete;
    SerialChannel& operator=(const SerialChannel&) = delete;
};

}} // namespace MyProt::Transport

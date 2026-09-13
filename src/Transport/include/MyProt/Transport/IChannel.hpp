// src/Transport/include/MyProt/Transport/IChannel.hpp
// 通道接口 — 异步回调模型 (C++11, ADR-0010 §2)
// 签名约定: 尾参回调; 回调在 io_context 线程触发, 实现方须保证回调保活 (shared_ptr 捕获)

#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Transport {

using Bytes = Core::Bytes;   // std::vector<uint8_t>

/// 连接完成回调 (成功或失败时恰好调用一次)
using ConnectHandler = std::function<void(Core::Expected<void>)>;

/// 请求-响应完成回调 (收到完整响应帧或失败时恰好调用一次)
using ReceiveHandler = std::function<void(Core::Expected<Core::Bytes>)>;

/// 通道抽象基类 — Tcp/Tls/Serial 具体通道的统一接口。
/// 单通道单请求: 同一时刻仅允许一笔在途 SendReceive (网关层按设备串行化)。
class IChannel {
public:
    virtual ~IChannel() = default;

    /// 异步建立物理连接; timeout 到期未完成则按 Timeout 失败
    virtual void Connect(const Core::ConnectionConfig& endpoint,
                         std::chrono::milliseconds timeout,
                         ConnectHandler handler) = 0;

    /// 异步请求-响应: 发送 request, 按 framingConfig 规则收取完整响应帧;
    /// framingConfig 为 null 时使用通道内置默认成帧
    virtual void SendReceive(const Bytes& request,
                             std::shared_ptr<const Core::FramingConfig> framingConfig,
                             std::chrono::milliseconds timeout,
                             ReceiveHandler handler) = 0;

    /// 断开并释放底层资源; 挂起操作以 ConnectionClosed 完成
    virtual void Disconnect() = 0;

    virtual bool IsConnected() const = 0;

    /// 最近一次失败的错误快照 (线程安全)
    virtual Core::Error GetLastError() const = 0;
};

/// 通道智能指针
using IChannelPtr = std::shared_ptr<IChannel>;

}} // namespace MyProt::Transport

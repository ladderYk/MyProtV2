// src/Northbound/include/MyProt/Northbound/MqttClient.hpp
// 北向 MQTT 3.1.1 客户端 — asio 异步传输层 (C++11)
// 职责边界 (ADR-0020):
//   MqttCodec   字节 <-> Core::Frame        (线格式)
//   MqttSession Core::Frame -> 协议动作      (会话状态机)
//   MqttClient  socket / 定时器 / 发送队列   (本类)
// 线程模型: 所有 IO 在调用方提供的 io_context 线程执行; Publish() 线程安全,
//           内部 post 到 IO 线程串行化写入 (socket 不支持并发 async_write)。

#pragma once
#include <asio.hpp>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MyProt/Core/FrameCodec.hpp"
#include "MyProt/Core/MqttConfig.hpp"
#include "MyProt/Northbound/MqttSession.hpp"

namespace MyProt { namespace Northbound {

class MqttClient : public std::enable_shared_from_this<MqttClient> {
public:
    typedef std::function<void()> ConnectedHandler;
    /// 收到 PUBLISH 时回调 (topic, payload); 在 IO 线程执行
    typedef std::function<void(const std::string&, const std::string&)> MessageHandler;
    /// 扩展回调 (topic, payload, qos, retain); 在 IO 线程执行
    typedef MqttSession::MessageHandlerEx MessageHandlerEx;

    MqttClient(asio::io_context& io, const Core::MqttConfig& cfg);
    ~MqttClient();

    /// 启动连接 (异步; 失败按指数退避自动重连, 直到 Stop)
    void Start();

    /// 停止并取消重连 (调用后不再有回调)
    void Stop();

    /// 发布一条消息; 未连接时返回 false (由调用方决定是否下轮重发)
    /// @note 线程安全
    bool Publish(const std::string& topic,
                 const std::string& payload,
                 int qos,
                 bool retain);

    /// 订阅主题 (QoS 0/1); 可重复调用, 同主题后者覆盖前者。
    /// 断线重连成功后由客户端自动重新订阅。
    /// @note 线程安全
    void Subscribe(const std::string& topic, int qos);

    /// 收到 PUBLISH 的回调; 未注册时消息被丢弃 (仍会回 QoS1 PUBACK)
    void SetOnMessage(MessageHandler handler);

    /// 扩展回调 (topic, payload, qos, retain); 与 SetOnMessage 互不替代
    void SetOnMessageEx(MessageHandlerEx handler) {
        _session.SetOnMessageEx(handler);
    }

    /// cleanSession 标志 — 必须在 Start() 前设置 (默认 true)
    void SetCleanSession(bool clean);

    /// 每次 (重新) 建立会话后回调 — 供上层清空"已发布"缓存, 确保重连后全量补发
    /// @note 在 IO 线程执行
    void SetOnConnected(ConnectedHandler handler);

    /// 遗嘱消息 (LWT) — 必须在 Start() 前设置; willTopic 为空表示不设置
    void SetWill(const std::string& willTopic,
                 const std::string& willPayload,
                 int willQos,
                 bool willRetain);

    bool IsConnected() const { return _connected.load(); }
    std::string LastError() const;

private:
    void BeginConnect();
    void OnResolved(const asio::error_code& ec,
                    asio::ip::tcp::resolver::results_type results);
    void OnTcpConnected(const asio::error_code& ec);
    void SendConnectPacket();
    void StartRead();
    void ReadSome();
    bool ProcessRx();                   // false = 已断开, 停止本轮解析
    bool HandleFrame(const Core::Frame& frame);
    void OnEstablished();               // CONNACK 通过后的连接建立收尾

    void EnqueuePacket(const std::vector<uint8_t>& packet);
    void StartWrite();

    void ArmKeepAlive();
    void FailAndReconnect(const std::string& reason);
    void ScheduleReconnect();
    void CloseSocket();
    void SetLastError(const std::string& msg);

    asio::io_context& _io;
    Core::MqttConfig _cfg;

    std::unique_ptr<Core::IFrameCodec> _codec;   // wire format (MqttCodec today)
    MqttSession _session;                       // protocol state machine, no I/O

    std::unique_ptr<asio::ip::tcp::socket> _socket;
    asio::ip::tcp::resolver _resolver;
    std::unique_ptr<asio::steady_timer> _reconnectTimer;
    std::unique_ptr<asio::steady_timer> _connectTimer;   // 解析+连接+CONNACK 总看门狗
    std::unique_ptr<asio::steady_timer> _keepAliveTimer;

    std::atomic<bool> _started;
    std::atomic<bool> _stopping;
    std::atomic<bool> _connected;
    ConnectedHandler _onConnected;

    // ── 以下状态仅在 IO 线程访问 ──
    std::vector<uint8_t> _rxBuf;
    std::deque<std::vector<uint8_t> > _txQueue;
    bool _reading;
    bool _writing;
    bool _attemptInFlight;       // 本轮 解析/连接/握手 未结束
    bool _reconnectScheduled;    // 已排队重连, 防 error 回调重复排程
    bool _pingOutstanding;
    int _reconnectDelayMs;
    uint64_t _droppedPackets;

    mutable std::mutex _errorMutex;
    std::string _lastError;

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;
};

}} // namespace MyProt::Northbound

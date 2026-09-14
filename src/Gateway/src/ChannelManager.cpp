// src/Gateway/src/ChannelManager.cpp — 通道管理器实现
// 异步连接 + 物理端点缓存 + 设备级通道管理 + 协议握手 (ADR-0002, ADR-0010)
#include "MyProt/Gateway/ChannelManager.hpp"
#include "MyProt/Engine/ExpressionEvaluator.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Core/Log.hpp"
#include "MyProt/Service/DeviceLifecycle.hpp"

#include <sstream>
#include <iomanip>

namespace MyProt { namespace Gateway {

namespace {

// ── 会话变量提取 ──────────────────────────────────────────────────
// 支持 resp[A:B] 字节切片 (存为连续大写 hex, 如 "0A1BFF") 与单字节 resp[N];
// 其他表达式回退为整数求值 (长度表达式)。
bool ExtractSessionVar(Core::ByteView response,
                       const std::string& expr,
                       std::string& out) {
    // 解析 resp[...]
    if (expr.size() > 5 && expr.compare(0, 5, "resp[") == 0) {
        size_t rb = expr.find(']', 5);
        if (rb == std::string::npos) return false;
        std::string inner = expr.substr(5, rb - 5);
        size_t colon = inner.find(':');
        long a = 0, b = 0;
        if (colon != std::string::npos) {
            a = std::strtol(inner.substr(0, colon).c_str(), nullptr, 0);
            b = std::strtol(inner.substr(colon + 1).c_str(), nullptr, 0);
        } else {
            a = std::strtol(inner.c_str(), nullptr, 0);
            b = a + 1;
        }
        if (a < 0) a = 0;
        if (b > static_cast<long>(response.size)) b = static_cast<long>(response.size);
        std::ostringstream oss;
        oss << std::uppercase << std::hex << std::setfill('0');
        for (long i = a; i < b; ++i) {
            oss << std::setw(2) << static_cast<int>(response[static_cast<size_t>(i)]);
        }
        out = oss.str();
        return true;
    }

    // 回退: 当作整数表达式 (如 "resp[2] + 1")
    Engine::ExpressionEvaluator eval;
    auto len = eval.EvaluateLength(expr, response);
    if (!len.has_value()) return false;
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0')
        << std::setw(2) << (len.value() & 0xFF);
    out = oss.str();
    return true;
}

// ── 握手请求帧渲染 ────────────────────────────────────────────────
// 按 token 序列拼出字节: 十六进制字面量直接解析; {Var} 若命中会话变量则
// 按其 hex 值展开; {Username}/{Password} 以原始 ASCII 字节注入。
// 返回 BuildError 时说明模板引用了未知变量。
Core::Expected<Core::Bytes> RenderHandshakeRequest(
    const std::vector<std::string>& requestTemplate,
    const Service::SessionContext& session,
    const Core::DeviceConfig& device) {

    Core::Bytes frame;
    for (const auto& token : requestTemplate) {
        if (!token.empty() && token[0] == '{') {
            std::string name = token.substr(1, token.size() - 2);
            // 去除可能的 ":fmt" / ":func:fmt" (握手阶段仅取变量名)
            size_t colon = name.find(':');
            if (colon != std::string::npos) name = name.substr(0, colon);

            std::string value;
            if (name == "Username" && device.username.has_value()) {
                value = device.username.value();
                frame.insert(frame.end(), value.begin(), value.end());
                continue;
            }
            if (name == "Password" && device.password.has_value()) {
                value = device.password.value();
                frame.insert(frame.end(), value.begin(), value.end());
                continue;
            }

            auto var = session.GetSessionVar(name);
            if (!var.has_value()) {
                return Core::Unexpected(Core::Error::Code::BuildError,
                    "握手模板变量未找到: " + name);
            }
            // 会话变量存为连续 hex, 逐字节解析
            const std::string& hex = var.value();
            for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                char buf[3] = { hex[i], hex[i + 1], 0 };
                frame.push_back(static_cast<uint8_t>(std::strtoul(buf, nullptr, 16)));
            }
        } else {
            // 十六进制字面量: 空格分隔字节
            const char* p = token.c_str();
            while (*p) {
                while (*p == ' ') ++p;
                if (!*p) break;
                char hex[3] = { p[0], p[1], 0 };
                frame.push_back(static_cast<uint8_t>(std::strtoul(hex, nullptr, 16)));
                p += 2;
            }
        }
    }
    return Core::Expected<Core::Bytes>(std::move(frame));
}

} // namespace

// ── 构造 ──

ChannelManager::ChannelManager(asio::io_context& io,
                               ProtocolLookup lookup,
                               ChannelFactory factory)
    : _io(io)
    , _lookup(std::move(lookup))
    , _factory(std::move(factory)) {}

// ── 设备注册 ──

void ChannelManager::RegisterDevice(const Core::DeviceConfig& device) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    auto& entry = _devices[device.id];
    entry.deviceConfig = device;
    entry.session = std::make_shared<Service::SessionContext>(device);
    entry.connecting = false;
    entry.failedStreak = 0;        // 新设备清零冷却计数
    entry.lastFailedAt = std::chrono::steady_clock::time_point();
}

void ChannelManager::RegisterDevices(const std::vector<Core::DeviceConfig>& devices) {
    for (const auto& d : devices) {
        RegisterDevice(d);
    }
}

void ChannelManager::ResetDevices(const std::vector<Core::DeviceConfig>& devices) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    _devices.clear();
    for (size_t i = 0; i < devices.size(); ++i) {
        ChannelEntry& entry = _devices[devices[i].id];
        entry.deviceConfig = devices[i];
        entry.session = std::make_shared<Service::SessionContext>(devices[i]);
        entry.connecting = false;
        entry.failedStreak = 0;    // 重置后清零冷却计数
        entry.lastFailedAt = std::chrono::steady_clock::time_point();
    }
}

// ── 物理端点标识计算 (ADR-0002 §4) ──

std::string ChannelManager::ComputeEndpointKey(
    const Core::ConnectionConfig& conn,
    const Core::TransportConfig& transport) {

    switch (transport.type) {
    case Core::TransportType::Tcp:
        return "tcp:" + conn.host + ":" + std::to_string(conn.port);
    case Core::TransportType::Tls:
        return "tls:" + conn.host + ":" + std::to_string(conn.port);
    case Core::TransportType::Serial:
        return "serial:" + transport.serial.portName;
    default:
        return "unknown";
    }
}

// ── 获取或创建通道 ──

void ChannelManager::GetOrCreateChannel(
    const std::string& deviceId,
    std::function<void(Core::Expected<ConnectResult>)> handler) {

    std::lock_guard<std::recursive_mutex> lock(_mutex);

    auto it = _devices.find(deviceId);

    // 冷却期短路: 上次连接失败后 2s 内的 GetOrCreateChannel 直接拒绝,
    // 避免多 PollGroup × 短轮询周期在断线期间并发 Connect 形成风暴。
    // (失败信息透传, PollingEngine 端按 IsRetryable → 走退避 → 自然消化)
    constexpr int kCooldownSeconds = 2;
    if (it != _devices.end() && it->second.failedStreak > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - it->second.lastFailedAt).count();
        if (elapsed < kCooldownSeconds) {
            handler(Core::Unexpected(Core::Error::Code::ConnectionClosed,
                "channel in cooldown", "device=" + deviceId));
            return;
        }
    }

    // 已连接 → 直接返回
    if (it != _devices.end() && it->second.channel &&
        it->second.channel->IsConnected() && !it->second.connecting) {
        ConnectResult result;
        result.channel = it->second.channel;
        // 注: P1 D busStrand 字段已撤回(2026-08-29,见 ADR-0011 §3.2)
        handler(Core::Expected<ConnectResult>(std::move(result)));
        return;
    }

    // 正在连接中 → 拒绝并发 (v1: 简单返回 Busy)
    if (it != _devices.end() && it->second.connecting) {
        handler(Core::Unexpected(Core::Error::Code::Busy,
            "设备正在连接中", "device=" + deviceId));
        return;
    }

    if (it == _devices.end()) {
        handler(Core::Unexpected(Core::Error::Code::DeviceNotFound,
            "设备未注册: " + deviceId,
            "请先调用 RegisterDevice 或 Initialize"));
        return;
    }

    auto& entry = it->second;
    auto protocolResult = _lookup(entry.deviceConfig.protocol);
    if (!protocolResult.has_value()) {
        handler(Core::UnexpectedType{protocolResult.error()});
        return;
    }

    auto protocol = std::make_shared<Core::ProtocolConfig>(std::move(protocolResult.value()));
    entry.connecting = true;
    // KI-04: New/Degraded → Connecting (Connected 不会重入此分支, 上面 fast-path 已返回)
    if (entry.session) {
        entry.session->SetLifecycle(Service::DeviceLifecycleState::Connecting);
    }
    PerformConnect(entry, *protocol, handler);
}

// ── 执行物理连接 ──

void ChannelManager::PerformConnect(
    ChannelEntry& entry,
    const Core::ProtocolConfig& protocol,
    std::function<void(Core::Expected<ConnectResult>)> handler) {

    // 有效端口解析 (ADR-0012 附录 A.1): device.connection.port=0 → 协议 defaultPort。
    // 端点键与 Connect 都必须使用解析后的有效端口 —— 通道不得自行回落到某个协议默认值
    // (否则 S7 漏填 port 时会连错端口, 且校验期看不出来)。
    Core::ConnectionConfig conn = entry.deviceConfig.connection;
    if (conn.port == 0) {
        if (protocol.transport.type == Core::TransportType::Tcp) {
            conn.port = protocol.transport.tcp.defaultPort;
        } else if (protocol.transport.type == Core::TransportType::Tls) {
            conn.port = protocol.transport.tls.defaultPort;
        }
    }

    auto endpointKey = ComputeEndpointKey(conn, protocol.transport);

    // 检查端点缓存 (共享总线, ADR-0002 §4)
    std::shared_ptr<Transport::IChannel> channel;
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        auto epIt = _endpointCache.find(endpointKey);
        if (epIt != _endpointCache.end() && epIt->second->IsConnected()) {
            channel = epIt->second;
        }
    }

    if (channel) {
        // 复用已建立的物理通道 — 不重复握手
        {
            std::lock_guard<std::recursive_mutex> lock(_mutex);
            entry.channel = channel;
            entry.connecting = false;
        }
        ConnectResult result;
        result.channel = channel;
        // 注: P1 D busStrand 已撤回(2026-08-29,见 ADR-0011 §3.2)
        handler(Core::Expected<ConnectResult>(std::move(result)));
        return;
    }

    // 创建新通道
    if (!_factory) {
        entry.connecting = false;
        handler(Core::Unexpected(Core::Error::Code::InternalError,
            "通道工厂未注入", "device=" + entry.deviceConfig.id));
        return;
    }

    channel = _factory(_io, entry.deviceConfig.id);
    if (!channel) {
        entry.connecting = false;
        handler(Core::Unexpected(Core::Error::Code::InternalError,
            "通道工厂创建失败", "device=" + entry.deviceConfig.id));
        return;
    }

    // 拷贝异步回调所需数据, 避免在回调中持锁访问 entry
    auto deviceConfig = entry.deviceConfig;
    deviceConfig.connection = conn;   // 有效端口 (port=0 → 协议 defaultPort, ADR-0012 附录A.1)
    auto session = entry.session;
    {
        std::lock_guard<std::recursive_mutex> lock(_mutex);
        entry.channel = channel;
        // 注: P1 D 新建 strand 已撤回(2026-08-29,见 ADR-0011 §3.2)
    }

    auto timeout = std::chrono::milliseconds(deviceConfig.connection.timeoutMs);
    auto self = this;
    auto deviceId = deviceConfig.id;
    auto handshakeProto = std::make_shared<Core::ProtocolConfig>(protocol);

    channel->Connect(deviceConfig.connection, timeout,
        [self, deviceId, deviceConfig, channel, endpointKey, session,
         handshakeProto, timeout, handler]
        (Core::Expected<void> connectResult) {

            if (!connectResult.has_value()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(self->_mutex);
                    auto it = self->_devices.find(deviceId);
                    if (it != self->_devices.end()) {
                        it->second.connecting = false;
                        // 冷却期计数: 失败 +1, 记录时刻
                        it->second.failedStreak++;
                        it->second.lastFailedAt = std::chrono::steady_clock::now();
                    }
                }
                // KI-04: 物理连接失败 → Degraded (连接级故障, 等待下一轮 GOC 重试)
                if (session) {
                    session->SetLifecycle(Service::DeviceLifecycleState::Degraded);
                }
                handler(Core::UnexpectedType{connectResult.error()});
                return;
            }

            // 缓存物理通道
            {
                std::lock_guard<std::recursive_mutex> lock(self->_mutex);
                self->_endpointCache[endpointKey] = channel;
            }
            Core::metrics::CounterInc(Core::metrics::kChannelConnectsTotal,
                                      Core::metrics::Device(deviceId));

            // 无握手步骤 → 直接成功
            if (handshakeProto->handshake.empty()) {
                {
                    std::lock_guard<std::recursive_mutex> lock(self->_mutex);
                    auto it = self->_devices.find(deviceId);
                    if (it != self->_devices.end()) {
                        it->second.connecting = false;
                        // 物理连接成功 → 清零冷却计数
                        it->second.failedStreak = 0;
                    }
                }
                // KI-04: 无握手 = Connecting → Connected
                if (session) {
                    session->SetLifecycle(Service::DeviceLifecycleState::Connected);
                }
                ConnectResult result;
                result.channel = channel;
                // 注: P1 D busStrand 已撤回(2026-08-29,见 ADR-0011 §3.2)
                handler(Core::Expected<ConnectResult>(std::move(result)));
                return;
            }

            // 执行协议握手序列
            self->PerformHandshake(channel, session, handshakeProto, deviceConfig,
                                   0, timeout,
                [self, deviceId, channel, endpointKey, session, handler]
                (Core::Expected<void> hsResult) {

                    {
                        std::lock_guard<std::recursive_mutex> lock(self->_mutex);
                        auto it = self->_devices.find(deviceId);
                        if (it != self->_devices.end()) {
                            it->second.connecting = false;
                            if (hsResult.has_value()) {
                                // 握手成功 → 清零冷却计数
                                it->second.failedStreak = 0;
                            } else {
                                // 握手失败 → 计入冷却 (与物理连接失败等价)
                                it->second.failedStreak++;
                                it->second.lastFailedAt = std::chrono::steady_clock::now();
                            }
                        }
                    }

                    if (!hsResult.has_value()) {
                        // 握手失败 → 拆除物理通道
                        {
                            std::lock_guard<std::recursive_mutex> lock(self->_mutex);
                            self->_endpointCache.erase(endpointKey);
                        }
                        channel->Disconnect();
                        // KI-04: 握手失败 (BuildError/InvalidResponse/ParseError) → Degraded
                        // (通道可重建后重试, 非永久禁用; 若属协议模板变量缺失等不可恢复
                        //  错误, 由 PollingEngine 重复失败累积时再考虑 Disabled)
                        if (session) {
                            session->SetLifecycle(Service::DeviceLifecycleState::Degraded);
                        }
                        handler(Core::UnexpectedType{hsResult.error()});
                        return;
                    }

                    // KI-04: 握手序列全部完成 → Connected
                    if (session) {
                        session->SetLifecycle(Service::DeviceLifecycleState::Connected);
                    }
                    ConnectResult result;
                    result.channel = channel;
                    // 注: P1 D busStrand 已撤回(2026-08-29,见 ADR-0011 §3.2)
                    handler(Core::Expected<ConnectResult>(std::move(result)));
                });
        });
}

// ── 协议握手顺序执行 ──

void ChannelManager::PerformHandshake(
    std::shared_ptr<Transport::IChannel> channel,
    std::shared_ptr<Service::SessionContext> session,
    std::shared_ptr<const Core::ProtocolConfig> protocol,
    const Core::DeviceConfig& device,
    size_t stepIndex,
    std::chrono::milliseconds defaultTimeout,
    std::function<void(Core::Expected<void>)> handler) {

    if (stepIndex >= protocol->handshake.size()) {
        handler(Core::VoidExpected());  // 全部握手步骤完成
        return;
    }

    const Core::HandshakeStep& step = protocol->handshake[stepIndex];

    auto request = RenderHandshakeRequest(step.requestTemplate, *session, device);
    if (!request.has_value()) {
        handler(Core::UnexpectedType{request.error()});
        return;
    }

    auto frame = std::make_shared<Core::Bytes>(std::move(request.value()));
    auto framing = step.framingOverride.has_value()
                       ? std::make_shared<Core::FramingConfig>(step.framingOverride.value())
                       : std::make_shared<Core::FramingConfig>(protocol->framing);
    auto timeout = step.timeoutMs > 0
                       ? std::chrono::milliseconds(step.timeoutMs)
                       : defaultTimeout;

    // 注: frame(request 字节)必须被捕获保活 — asio::async_write 仅持有其指针、
    //     不拷贝数据; 若 frame 随本函数返回析构, 异步发送完成时将悬垂访问崩溃。
    channel->SendReceive(*frame, framing, timeout,
        [this, channel, session, protocol, device, stepIndex, defaultTimeout, handler, frame]
        (Core::Expected<Core::Bytes> respResult) {

            if (!respResult.has_value()) {
                handler(Core::UnexpectedType{respResult.error()});
                return;
            }

            const Core::Bytes& response = respResult.value();
            const Core::HandshakeStep& step = protocol->handshake[stepIndex];
            Core::ByteView respView(response);

            // 校验成功条件
            if (!step.validCondition.empty()) {
                Engine::ExpressionEvaluator eval;
                auto cond = eval.EvaluateCondition(step.validCondition, respView);
                if (!cond.has_value()) {
                    handler(Core::UnexpectedType{cond.error()});
                    return;
                }
                if (!cond.value()) {
                    handler(Core::Unexpected(Core::Error::Code::InvalidResponse,
                        "握手步骤 \"" + step.name + "\" 校验失败: " + step.validCondition));
                    return;
                }
            }

            // 提取会话变量
            if (!step.sessionExtractExpr.empty() && !step.sessionVariable.empty()) {
                std::string extracted;
                if (!ExtractSessionVar(respView, step.sessionExtractExpr, extracted)) {
                    handler(Core::Unexpected(Core::Error::Code::ParseError,
                        "握手步骤 \"" + step.name + "\" 会话变量提取失败: " +
                        step.sessionExtractExpr));
                    return;
                }
                session->SetSessionVar(step.sessionVariable, extracted);
            }

            // 递归执行下一步
            PerformHandshake(channel, session, protocol, device,
                             stepIndex + 1, defaultTimeout, handler);
        });
}

// ── 关闭所有通道 ──

void ChannelManager::ShutdownAll() {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    for (auto& pair : _endpointCache) {
        if (pair.second) {
            pair.second->Disconnect();
        }
    }
    _endpointCache.clear();
    for (auto& pair : _devices) {
        pair.second.channel.reset();
        pair.second.connecting = false;
    }
}

// ── 获取会话上下文 ──

std::shared_ptr<Service::SessionContext> ChannelManager::GetSession(
    const std::string& deviceId) {
    std::lock_guard<std::recursive_mutex> lock(_mutex);
    auto it = _devices.find(deviceId);
    if (it != _devices.end()) {
        return it->second.session;
    }
    return nullptr;
}

}} // namespace MyProt::Gateway

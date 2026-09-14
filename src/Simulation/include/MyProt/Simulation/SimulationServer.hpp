// src/Simulation/include/MyProt/Simulation/SimulationServer.hpp
// 配置驱动仿真服务器 — 协议 JSON 的反向闭环 (Phase 1)
//
//   收帧 → TemplateMatcher 反向识别 (操作名+变量)
//        → simulation.operations 声明的数据方向 (read/write) 操作数据区
//        → ResponseSynthesizer 按 responseParser 规格合成应答
//
// 报文格式知识全部来自 ProtocolConfig (transport/framing/operations),
// 本服务器只按 simulation 节声明摆弄数据区 — 对任意自描述协议零硬编码。
//
// Phase 1 边界: TCP 监听 (loopback), LengthField/Fixed 切帧;
// Silence (串口 RTU) 仿真不在本期范围。

#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Simulation/TemplateMatcher.hpp"
#include "MyProt/Simulation/ResponseSynthesizer.hpp"
#include "MyProt/Simulation/SimulationDataStore.hpp"

namespace MyProt { namespace Simulation {

class SimulationServer {
public:
    /// protocol 须在服务器生命周期内保持有效；simOverride 须来自 ServerConfig.simulation
        /// (从 server.json 解析, 与 ProtocolConfig 解耦)。
    /// 注: 不再共享外部 io_context — 服务器自持独立 io_context 运行于专用线程,
    /// 保证 Stop() 跨线程 io.stop() 即刻生效 (修复热重载挂起, 见 KI 登记)。
    explicit SimulationServer(const Core::ProtocolConfig& protocol,
                              const Core::SimulationConfig& simOverride);
    ~SimulationServer();

    SimulationDataStore& Store() { return _store; }

    /// 监听端口 (Start 成功后有效; 0 = 未启动)
    std::uint16_t ListenPort() const { return _listenPort; }

    /// 预编译匹配器与应答规格并启动监听 (simulation.listenPort, 仅环回)。
    /// 未启用仿真节 / 端口绑定失败 → false + err 说明。
    bool Start(std::string& err);

    /// 停止监听并结束工作线程 (阻塞至线程退出; 经 io.stop() 即刻唤醒, 无死等)
    void Stop();

private:
    /// 每操作的预编译执行计划
    struct SimPlan {
        Core::SimOperationConfig cfg;
        ResponseSynthesizer::Spec spec; ///< 应答规格 (预解析缓存)
    };

    typedef std::shared_ptr<asio::ip::tcp::socket> SocketPtr;

    /// 全异步 accept/read 链 (运行于自有 io_context 专用线程);
    /// 同步阻塞模型在 Windows 上无法被另一线程 close() 唤醒, 禁止回退。
    void DoAccept();
    void DoRead(const SocketPtr& socket,
                const std::shared_ptr<std::vector<std::uint8_t> >& buf);
    void HandleFrame(const SocketPtr& socket,
                     const std::vector<std::uint8_t>& frame);
    /// 从累积缓冲切出一个完整帧; 有则写入 frame 并返回消耗字节数, 无返回 0
    std::size_t TryExtractFrame(std::vector<std::uint8_t>& buf,
                                std::vector<std::uint8_t>& frame) const;

    const Core::ProtocolConfig& _protocol;
        const Core::SimulationConfig& _sim;  // 引用外部传入的 sim (server.json)
    TemplateMatcher _matcher;
    SimulationDataStore _store;
    std::map<std::string, SimPlan> _plans;              ///< sim 声明操作的执行计划
    std::unique_ptr<asio::io_context> _ioCtx;           ///< 自有 io_context — io.stop() 线程安全
    std::unique_ptr<asio::ip::tcp::acceptor> _acceptor;
    std::unique_ptr<std::thread> _thread;
    std::atomic<bool> _running;
    std::uint16_t _listenPort;
};

}} // namespace MyProt::Simulation

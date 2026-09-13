// src/Simulation/src/SimulationServer.cpp
// 配置驱动仿真服务器实现 — 见 SimulationServer.hpp 文件头注释

#include "MyProt/Simulation/SimulationServer.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "MyProt/Transport/LengthFieldFrameParser.hpp"

namespace MyProt { namespace Simulation {

using asio::ip::tcp;

namespace {

/// expr 是否以标识符形式引用 name (词边界匹配 — 避免 StartByteAddressX 误命中)
bool ExprReferences(const std::string& expr, const std::string& name) {
    if (expr.empty() || name.empty()) return false;
    std::size_t p = 0;
    while ((p = expr.find(name, p)) != std::string::npos) {
        const std::size_t e = p + name.size();
        const bool leftOK = (p == 0)
            || !(std::isalnum(static_cast<unsigned char>(expr[p - 1]))
                 || expr[p - 1] == '_');
        const bool rightOK = (e >= expr.size())
            || !(std::isalnum(static_cast<unsigned char>(expr[e]))
                 || expr[e] == '_');
        if (leftOK && rightOK) return true;
        p = e;
    }
    return false;
}

/// 由协议 JSON 推导 server.json 未声明的仿真描述符 (单一真源):
///   kind       ← 操作自身的 kind
///   addressVar ← outputs 中 expr 引用合约键 StartByteAddress 的 derivedLength 名
///   countVar   ← outputs 中 expr 引用合约键 ByteCount 的 derivedLength 名
/// 已显式声明的字段不被覆盖 (server.json 优先, 便于协议无法表达时的例外配置).
void DeriveSimDescriptors(const Core::ProtocolConfig& proto,
                          const Core::OperationConfig& op,
                          Core::SimOperationConfig& cfg) {
    if (cfg.kind.empty()) cfg.kind = op.kind;
    const std::string addrKey = Core::StartByteAddressVariableName();
    const std::string cntKey = Core::ByteCountVariableName();
    const std::unordered_map<std::string, Core::VariableConfig>* tables[2] = {
        &op.outputs, &proto.outputs };
    for (int t = 0; t < 2; ++t) {
        for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it =
                 tables[t]->begin(); it != tables[t]->end(); ++it) {
            if (!it->second.isDerivedLength()) continue;
            if (cfg.addressVar.empty() && ExprReferences(it->second.expr, addrKey)) {
                cfg.addressVar = it->first;
            }
            if (cfg.countVar.empty() && ExprReferences(it->second.expr, cntKey)) {
                cfg.countVar = it->first;
            }
        }
    }
}

} // namespace

SimulationServer::SimulationServer(const Core::ProtocolConfig& protocol,
                                   const Core::SimulationConfig& simOverride)
    : _protocol(protocol)
    , _sim(simOverride)
    , _matcher(protocol)
    , _store(static_cast<std::size_t>(
          simOverride.registerCount > 0 ? simOverride.registerCount : 1))
    , _running(false)
    , _listenPort(0) {
}

SimulationServer::~SimulationServer() {
    Stop();
}

bool SimulationServer::Start(std::string& err) {
    if (_sim.listenPort == 0) {
        err = "simulation 节未启用 (listenPort == 0)";
        return false;
    }

    // 预编译: 模板匹配器 + 每操作的应答规格
    _matcher.Compile();
    // 模板形状歧义告警 (不阻断 — Match 仍按编译序取首个)
    for (std::size_t i = 0; i < _matcher.Ambiguities().size(); ++i) {
        std::fprintf(stderr, "[SimMatcher][warn] protocol '%s': %s\n",
                     _protocol.protocolName.c_str(),
                     _matcher.Ambiguities()[i].c_str());
    }
    _plans.clear();
    // 遍历协议全部操作 (协议 JSON 为单一真源); server.json 的同名条目仅作覆盖,
    // 缺失描述符由 DeriveSimDescriptors 补全 —— 故 server.json 通常只需
    // listenPort / initialValues, 无需重复协议已表达的方向与变量名.
    std::unordered_map<std::string, Core::OperationConfig>::const_iterator pit;
    for (pit = _protocol.operations.begin();
         pit != _protocol.operations.end(); ++pit) {
        const std::string& opName = pit->first;

        Core::SimOperationConfig cfg;
        std::map<std::string, Core::SimOperationConfig>::const_iterator sit =
            _sim.operations.find(opName);
        if (sit != _sim.operations.end()) cfg = sit->second;   // 显式声明覆盖推导值
        DeriveSimDescriptors(_protocol, pit->second, cfg);

        SimPlan plan;
        plan.cfg = cfg;
        ResponseSynthesizer::ParseSpec(pit->second, plan.spec);
        if (plan.cfg.kind != "read" && plan.cfg.kind != "write") continue;
        if (plan.cfg.addressVar.empty()) continue;
        if (plan.cfg.kind == "read" && plan.cfg.countVar.empty()) continue;
        _plans[opName] = plan;
    }

    // 数据区初值
    std::map<std::string, std::uint32_t>::const_iterator init;
    for (init = _sim.initialValues.begin();
         init != _sim.initialValues.end(); ++init) {
        const long addr = std::strtol(init->first.c_str(), 0, 10);
        if (addr >= 0 && addr <= 65535) {
            _store.WriteRegisterValue(static_cast<std::uint16_t>(addr),
                                      static_cast<std::uint16_t>(init->second & 0xFFFF));
        }
    }

    // 监听 (仅环回 — 仿真器默认不暴露到外部网络)
    // 自有 io_context + 全异步模型: Stop() 跨线程 io.stop() 即刻唤醒工作线程;
    // 旧同步阻塞 accept 无法被另一线程 close() 唤醒 (WinSock 语义),
    // 曾导致热重载 Stop→join 永久挂起并拖死 WebApi 单线程事件循环。
    _ioCtx.reset(new asio::io_context());
    tcp::endpoint ep(tcp::v4(), _sim.listenPort);
    std::unique_ptr<tcp::acceptor> acc(new tcp::acceptor(*_ioCtx));
    asio::error_code ec;
    // 注: 失败路径不手动 reset _ioCtx — 局部 acc 仍持有它, 交由成员
    // 反向析构顺序 (_acceptor 先于 _ioCtx) 保证安全。
    acc->open(tcp::v4(), ec);
    if (ec) { err = ec.message(); return false; }
    // 注: 仅非 Windows 启用 reuse_address — WinSock 语义下它允许劫持
    // 仍处于 LISTEN 的端口, 两个进程同时监听会把连接静默导向旧实例
    // (数据串扰/无应答), 冲突应以 bind 失败显式暴露而非静默共存。
#ifndef _WIN32
    acc->set_option(asio::socket_base::reuse_address(true), ec);
#endif
    acc->bind(ep, ec);
    if (ec) { err = ec.message(); return false; }
    acc->listen(asio::socket_base::max_connections, ec);
    if (ec) { err = ec.message(); return false; }

    _acceptor = std::move(acc);
    _listenPort = _sim.listenPort;
    _running = true;
    DoAccept();
    _thread.reset(new std::thread([this]() { _ioCtx->run(); }));
    return true;
}

void SimulationServer::Stop() {
    if (!_running.exchange(false)) return;
    if (_ioCtx) {
        _ioCtx->stop();     // io_context::stop 线程安全 (asio 保证): run() 立即返回
    }
    if (_thread && _thread->joinable()) _thread->join();
    _thread.reset();
    _acceptor.reset();      // 工作线程已退出, 此刻释放无并发访问
    _ioCtx.reset();
}

void SimulationServer::DoAccept() {
    SocketPtr sock(new tcp::socket(*_ioCtx));
    _acceptor->async_accept(*sock,
        [this, sock](const asio::error_code& ec) {
            if (ec || !_running.load()) return;   // 停止/异常: 不再续链
            DoRead(sock,
                   std::shared_ptr<std::vector<std::uint8_t> >(
                       new std::vector<std::uint8_t>()));
            DoAccept();                            // 继续接受下一连接
        });
}

void SimulationServer::DoRead(
        const SocketPtr& socket,
        const std::shared_ptr<std::vector<std::uint8_t> >& buf) {
    std::shared_ptr<std::vector<std::uint8_t> > chunk(
        new std::vector<std::uint8_t>(4096));
    socket->async_read_some(asio::buffer(&(*chunk)[0], chunk->size()),
        [this, socket, buf, chunk](const asio::error_code& ec, std::size_t n) {
            if (ec || n == 0) {                    // 对端关闭/出错 → 连接结束
                asio::error_code ignore;
                socket->shutdown(tcp::socket::shutdown_both, ignore);
                socket->close(ignore);
                return;
            }
            buf->insert(buf->end(), chunk->begin(),
                        chunk->begin() + static_cast<std::ptrdiff_t>(n));

            // 切帧循环: 缓冲里可能有粘包; 应答为环回小帧, handler 内同步写回
            for (;;) {
                std::vector<std::uint8_t> frame;
                const std::size_t consumed = TryExtractFrame(*buf, frame);
                if (consumed == 0) break;
                HandleFrame(socket, frame);
                buf->erase(buf->begin(),
                           buf->begin() + static_cast<std::ptrdiff_t>(consumed));
                if (buf->empty()) break;
            }
            DoRead(socket, buf);                   // 继续读下一批
        });
}

std::size_t SimulationServer::TryExtractFrame(std::vector<std::uint8_t>& buf,
                                              std::vector<std::uint8_t>& frame) const {
    if (buf.empty()) return 0;
    Core::ByteView view(&buf[0], buf.size());

    switch (_protocol.framing.type) {
        case Core::FramingType::LengthField: {
            Transport::LengthFieldFrameParser parser(_protocol.framing.lengthField);
            Transport::Expected<Transport::FrameParseResult> r = parser.Parse(view, 0);
            if (!r.has_value() || r.value().needMoreData) return 0;
            frame = r.value().frame;
            return r.value().consumedBytes;
        }
        case Core::FramingType::Fixed: {
            const std::size_t len =
                _protocol.framing.fixed.fixedLength > 0
                    ? static_cast<std::size_t>(_protocol.framing.fixed.fixedLength) : 0;
            if (len == 0 || buf.size() < len) return 0;
            frame.assign(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(len));
            return len;
        }
        default:
            return 0; // Silence/Message: Phase 1 TCP 仿真不支持
    }
}

void SimulationServer::HandleFrame(const SocketPtr& socket,
                                   const std::vector<std::uint8_t>& frame) {
    Core::ByteView view(frame.empty() ? 0 : &frame[0], frame.size());

    const TemplateMatch m = _matcher.Match(view);
    if (!m.matched) return; // 未识别请求静默忽略

    std::map<std::string, SimPlan>::const_iterator it = _plans.find(m.operation);
    if (it == _plans.end()) return;
    const SimPlan& plan = it->second;

    // 变量取值辅助
    std::map<std::string, std::uint32_t>::const_iterator av =
        m.variables.find(plan.cfg.addressVar);
    if (av == m.variables.end()) return;
    const std::uint16_t address = static_cast<std::uint16_t>(av->second & 0xFFFF);

    std::vector<std::uint8_t> data;
    if (plan.cfg.kind == "read") {
        std::map<std::string, std::uint32_t>::const_iterator cv =
            m.variables.find(plan.cfg.countVar);
        if (cv == m.variables.end()) return;
        const std::uint32_t count32 = cv->second;
        // v1.29: 原为写死的 Modbus 单读上限 125 (超出即静默不应答) — 非 Modbus 协议
        //   读 >125 会一直无响应直到采集侧超时. 改为只做地址空间边界校验 (count 须能
        //   放进 16 位地址空间), 数据区越界统一由 _store 返回空处理 (见下方 data.empty()).
        if (count32 == 0 || count32 > 65535u) return;
        data = _store.ReadRegisters(address, static_cast<std::uint16_t>(count32));
        if (data.empty()) return;                 // 数据区越界: 不应答
    } else { // write
        if (plan.cfg.dataOffset < 0 ||
            static_cast<std::size_t>(plan.cfg.dataOffset) >= frame.size()) return;
        data.assign(frame.begin() + plan.cfg.dataOffset, frame.end());
        if (data.empty() || data.size() % 2 != 0) return;
        _store.WriteRegisters(address, data);
        data.clear(); // 写应答不带数据区 (echo 帧由合成器按 spec 生成)
    }

    // 应答合成: 自定义模板优先, 否则 echo 反向合成
    std::vector<std::uint8_t> resp;
    if (!plan.cfg.responseTemplate.empty()) {
        if (!ResponseSynthesizer::SynthesizeFromTemplate(
                plan.cfg.responseTemplate, frame, data,
                _protocol.framing, resp)) {
            return; // 模板文法/越界: 不应答 (加载期校验应已拦截)
        }
    } else {
        resp = ResponseSynthesizer::Synthesize(plan.spec, frame, data,
                                               _protocol.framing);
    }
    if (resp.empty()) return;

    asio::error_code ec;
    asio::write(*socket, asio::buffer(resp), ec);
}

}} // namespace MyProt::Simulation

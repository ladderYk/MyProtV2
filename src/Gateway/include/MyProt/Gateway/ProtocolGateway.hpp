// src/Gateway/include/MyProt/Gateway/ProtocolGateway.hpp
// 协议网关门面 — 上层 (PollingEngine / WebApi) 通过此对象访问设备 (modules/05_Gateway.md §5.1)

#pragma once
#include <memory>
#include <vector>
#include <asio.hpp>

#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Gateway/ChannelManager.hpp"
#include "MyProt/Gateway/TagReader.hpp"
#include "MyProt/Gateway/TagGrouper.hpp"
#include "MyProt/Gateway/ProtocolLookup.hpp"

namespace MyProt { namespace Gateway {

/// 协议网关门面 — 封装 ChannelManager + TagReader + TagGrouper
/// 上层模块 (Polling / WebApi) 的唯一交互入口
class ProtocolGateway {
public:
    ProtocolGateway(asio::io_context& io,
                    ProtocolLookup lookup,
                    ChannelFactory factory);

    /// 注册设备 (从 ConfigRoot 导入设备配置到 ChannelManager)
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    /// 获取 ChannelManager
    ChannelManager& GetChannelManager() { return *_channelMgr; }

    /// 获取 TagReader
    TagReader& GetTagReader() { return *_tagReader; }

    /// 获取 TagGrouper
    TagGrouper& GetTagGrouper() { return *_tagGrouper; }

    /// 获取协议查找函数
    const ProtocolLookup& GetProtocolLookup() const { return _lookup; }

    /// 优雅关闭所有通道
    void Shutdown();

private:
    asio::io_context& _io;
    ProtocolLookup _lookup;
    std::unique_ptr<ChannelManager> _channelMgr;
    std::unique_ptr<TagReader> _tagReader;
    std::unique_ptr<TagGrouper> _tagGrouper;
};

}} // namespace MyProt::Gateway

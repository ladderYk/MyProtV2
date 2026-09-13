// src/Gateway/src/ProtocolGateway.cpp — 协议网关门面实现
#include "MyProt/Gateway/ProtocolGateway.hpp"

namespace MyProt { namespace Gateway {

ProtocolGateway::ProtocolGateway(asio::io_context& io,
                                 ProtocolLookup lookup,
                                 ChannelFactory factory)
    : _io(io)
    , _lookup(std::move(lookup)) {
    _channelMgr.reset(new ChannelManager(_io, _lookup, std::move(factory)));
    _tagReader.reset(new TagReader());
    _tagGrouper.reset(new TagGrouper());
}

void ProtocolGateway::RegisterDevices(const std::vector<Core::DeviceConfig>& devices) {
    if (_channelMgr) {
        _channelMgr->RegisterDevices(devices);
    }
}

void ProtocolGateway::Shutdown() {
    if (_channelMgr) {
        _channelMgr->ShutdownAll();
    }
}

}} // namespace MyProt::Gateway

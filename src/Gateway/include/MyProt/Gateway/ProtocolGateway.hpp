// src/Gateway/include/MyProt/Gateway/ProtocolGateway.hpp
// Protocol gateway facade - upper layers (PollingEngine / WebApi) access devices through this object (modules/05_Gateway.md §5.1)

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

/// Protocol gateway facade - wraps ChannelManager + TagReader + TagGrouper
/// The sole interaction entry point for upper modules (Polling / WebApi)
class ProtocolGateway {
public:
    ProtocolGateway(asio::io_context& io,
                    ProtocolLookup lookup,
                    ChannelFactory factory);

    /// Register devices (import device configs from ConfigRoot into ChannelManager)
    void RegisterDevices(const std::vector<Core::DeviceConfig>& devices);

    /// Get the ChannelManager
    ChannelManager& GetChannelManager() { return *_channelMgr; }

    /// Get the TagReader
    TagReader& GetTagReader() { return *_tagReader; }

    /// Get the TagGrouper
    TagGrouper& GetTagGrouper() { return *_tagGrouper; }

    /// Get the protocol lookup function
    const ProtocolLookup& GetProtocolLookup() const { return _lookup; }

    /// Gracefully shut down all channels
    void Shutdown();

private:
    asio::io_context& _io;
    ProtocolLookup _lookup;
    std::unique_ptr<ChannelManager> _channelMgr;
    std::unique_ptr<TagReader> _tagReader;
    std::unique_ptr<TagGrouper> _tagGrouper;
};

}} // namespace MyProt::Gateway

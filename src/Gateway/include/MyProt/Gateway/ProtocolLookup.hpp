// src/Gateway/include/MyProt/Gateway/ProtocolLookup.hpp
// Protocol lookup interface - decouples Gateway from the config store (modules/05_Gateway.md)

#pragma once
#include <functional>
#include <string>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Gateway {

/// Protocol-config lookup - the implementation is injected by the upper layer (Service/ConfigStore)
/// Returns Expected<ProtocolConfig>; returns an Error when the protocol does not exist
using ProtocolLookup = std::function<Core::Expected<Core::ProtocolConfig>(const std::string& protocolName)>;

}} // namespace MyProt::Gateway

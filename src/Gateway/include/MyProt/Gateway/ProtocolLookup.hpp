// src/Gateway/include/MyProt/Gateway/ProtocolLookup.hpp
// 协议查找接口 — 解耦 Gateway 与配置存储 (modules/05_Gateway.md)

#pragma once
#include <functional>
#include <string>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"

namespace MyProt { namespace Gateway {

/// 协议配置查找 — 由上层 (Service/ConfigStore) 注入实现
/// 返回 Expected<ProtocolConfig>; 协议不存在时返回 Error
using ProtocolLookup = std::function<Core::Expected<Core::ProtocolConfig>(const std::string& protocolName)>;

}} // namespace MyProt::Gateway

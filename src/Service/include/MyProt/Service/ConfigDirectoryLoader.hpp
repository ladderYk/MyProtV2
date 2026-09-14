// src/Service/include/MyProt/Service/ConfigDirectoryLoader.hpp
// 目录布局加载器 — 生产启动模式的配置入口 (Config_Schema §1)

#pragma once
#include <string>
#include <vector>
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // LoadedConfig.server
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Service {

/// ── 配置目录布局契约名 (磁盘路径的单一真源) ──
///   ConfigStore 与 ConfigDirectoryLoader 共用 — 避免同一组目录名/文件名两处各写一份.
///   分隔符统一用 '/'：Win32 与 MSVC CRT 的 _findfirst / ifstream 均接受正斜杠,
///   分隔符统一为 '/' + 通配符 (不得再有 '\' 写法).
namespace ConfigLayout {
const char* const kProtocolsDirPath = "/protocols";    // 协议文件目录 (含前置分隔符)
const char* const kTagsFilePath     = "/tags.json";    // 设备 + 标签 + 韧性 + 管理面
const char* const kServerFilePath   = "/server.json";  // 全局服务端 (仿真) 配置
}

/// 目录加载产物: 协议集 + 配置根 + 服务端 (均已通过深度校验与解析)
struct LoadedConfig {
    std::vector<Core::ProtocolConfig> protocols;
    Core::ConfigRoot root;
    // 全局服务端配置 (仿真 + 未来 alertSink/webhook). 协议层不承载服务端行为.
    // server.json 可选缺失 — SimulationConfig 字段全部默认 (listenPort=0 即关闭仿真).
    Core::ServerConfig server;

    LoadedConfig() {}
};

/// 目录布局加载器 (Config_Schema §1):
///   <dir>/protocols/*.json — 协议文件集 (每文件一个协议)
///   <dir>/tags.json        — 设备 + 标签 + 全局韧性 + WebApi
///   <dir>/server.json      — 全局服务端 (仿真) 配置; 缺失 = 默认关闭
///
/// 校验(十五项规则 + 跨文件引用)与解析一体 (单次 JSON 解析):
/// 对每个文件调用 validateAndParse* — 深度校验 (Fail-Fast, 错误携带文件名)
/// 与 POCO 转换一次完成; 根配置阶段复用已解析协议集, 不重复解析。
/// 校验期 Warning 输出到 stderr, 不阻断加载。
class ConfigDirectoryLoader {
public:
    /// @param configDir              配置目录
    /// @param supportedSchemaVersion 版本门禁代际 (ADR-0005)
    static Core::Expected<LoadedConfig> Load(const std::string& configDir,
                                             int supportedSchemaVersion);
};

}} // namespace MyProt::Service

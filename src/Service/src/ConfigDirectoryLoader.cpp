// src/Service/src/ConfigDirectoryLoader.cpp
// 目录布局加载器实现 — 校验与解析一体, 生产启动模式入口
// 单次 JSON 解析: validateAndParse* 同时产出校验结果与 POCO, 不做二次解析。

#include "MyProt/Service/ConfigDirectoryLoader.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>     // 别名归一化的标识符扫描

#include "MyProt/Core/Platform.hpp"     // 跨平台目录枚举
#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // 解析 server.json
#include "ConfigInternals.hpp"            // simulation 段共用解析/校验
#include "ConfigJsonText.hpp"             // 配置文件 JSON: 允许注释 + 容忍 BOM
#include <nlohmann/json.hpp>

namespace MyProt { namespace Service {

namespace {

bool ReadTextFile(const std::string& path, std::string& out, std::string& err) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) {
        err = "无法打开文件: " + path;
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/// 枚举 dir 下 *.json 文件名 (不含路径, 已排序保证加载顺序稳定)
std::vector<std::string> ListJsonFiles(const std::string& dir) {
    return Core::Platform::ListFiles(dir, ".json");
}

// ── server.json "mqtt" 段 (北向发布) ────────────────────────
// 接受 "host" / "host:port" / "tcp://host:port" / "mqtt://host:port"。
// IPv6 字面量 (::1) 不支持简写, 需直接用 host/port 两个字段。
bool ParseMqttBroker(const std::string& raw, std::string& host, int& port) {
    std::string s = raw;
    const char* schemes[2] = { "tcp://", "mqtt://" };
    for (int i = 0; i < 2; ++i) {
        const std::string scheme(schemes[i]);
        if (s.compare(0, scheme.size(), scheme) == 0) {
            s = s.substr(scheme.size());
            break;
        }
    }
    if (s.empty()) return false;
    const std::size_t colon = s.rfind(':');
    if (colon != std::string::npos && s.find(':') == colon) {
        host = s.substr(0, colon);
        const std::string p = s.substr(colon + 1);
        if (host.empty() || p.empty()) return false;
        int v = 0;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(p[i]))) return false;
            v = v * 10 + (p[i] - '0');
        }
        port = v;
        return true;
    }
    host = s;
    return !host.empty();
}

bool ParseMqttDoc(const nlohmann::json& doc, Core::MqttConfig& out, std::string& err) {
    if (!doc.is_object()) { err = "须为对象"; return false; }
    if (doc.contains("enabled")) {
        if (!doc["enabled"].is_boolean()) { err = "enabled 须为 bool"; return false; }
        out.enabled = doc["enabled"].get<bool>();
    }
    if (doc.contains("broker")) {
        if (!doc["broker"].is_string()) { err = "broker 须为字符串"; return false; }
        if (!ParseMqttBroker(doc["broker"].get<std::string>(), out.host, out.port)) {
            err = "broker 须为 \"host\" / \"host:port\" / \"tcp://host:port\"";
            return false;
        }
    }
    if (doc.contains("host")) {
        if (!doc["host"].is_string()) { err = "host 须为字符串"; return false; }
        out.host = doc["host"].get<std::string>();
    }
    if (doc.contains("port")) {
        if (!doc["port"].is_number_integer()) { err = "port 须为整数"; return false; }
        out.port = doc["port"].get<int>();
    }
    if (doc.contains("clientId")) {
        if (!doc["clientId"].is_string()) { err = "clientId 须为字符串"; return false; }
        out.clientId = doc["clientId"].get<std::string>();
    }
    if (doc.contains("username")) {
        if (!doc["username"].is_string()) { err = "username 须为字符串"; return false; }
        out.username = doc["username"].get<std::string>();
    }
    if (doc.contains("password")) {
        if (!doc["password"].is_string()) { err = "password 须为字符串"; return false; }
        out.password = doc["password"].get<std::string>();
    }
    if (doc.contains("keepAliveSec")) {
        if (!doc["keepAliveSec"].is_number_integer()) {
            err = "keepAliveSec 须为整数"; return false;
        }
        out.keepAliveSec = doc["keepAliveSec"].get<int>();
    }
    if (doc.contains("topicPrefix")) {
        if (!doc["topicPrefix"].is_string()) { err = "topicPrefix 须为字符串"; return false; }
        out.topicPrefix = doc["topicPrefix"].get<std::string>();
    }
    if (doc.contains("publishMode")) {
        if (!doc["publishMode"].is_string()) { err = "publishMode 须为字符串"; return false; }
        out.publishMode = doc["publishMode"].get<std::string>();
    }
    if (doc.contains("publishIntervalMs")) {
        if (!doc["publishIntervalMs"].is_number_integer()) {
            err = "publishIntervalMs 须为整数"; return false;
        }
        out.publishIntervalMs = doc["publishIntervalMs"].get<int>();
    }
    if (doc.contains("qos")) {
        if (!doc["qos"].is_number_integer()) { err = "qos 须为整数"; return false; }
        out.qos = doc["qos"].get<int>();
    }
    if (doc.contains("retain")) {
        if (!doc["retain"].is_boolean()) { err = "retain 须为 bool"; return false; }
        out.retain = doc["retain"].get<bool>();
    }
    // topic 尾部分隔符统一去掉, 避免生成 "myprot//device/tag"
    while (!out.topicPrefix.empty()
           && out.topicPrefix[out.topicPrefix.size() - 1] == '/') {
        out.topicPrefix.erase(out.topicPrefix.size() - 1);
    }
    return true;
}

bool ValidateMqttConfig(const Core::MqttConfig& c, std::string& err) {
    if (!c.enabled) return true;   // 禁用时允许预留字段, 不阻塞既有配置
    if (c.host.empty()) { err = "host 不能为空"; return false; }
    if (c.port < 1 || c.port > 65535) { err = "port 须在 1..65535"; return false; }
    if (c.clientId.empty()) { err = "clientId 不能为空"; return false; }
    if (c.keepAliveSec < 0 || c.keepAliveSec > 3600) {
        err = "keepAliveSec 须在 0..3600 (0 = 关闭心跳)"; return false;
    }
    if (c.topicPrefix.empty()) { err = "topicPrefix 不能为空"; return false; }
    if (c.topicPrefix.find('#') != std::string::npos
            || c.topicPrefix.find('+') != std::string::npos) {
        err = "topicPrefix 不能包含通配符 # / +"; return false;
    }
    if (c.publishMode != "changed" && c.publishMode != "all") {
        err = "publishMode 须为 changed 或 all"; return false;
    }
    if (c.publishIntervalMs < 100 || c.publishIntervalMs > 3600000) {
        err = "publishIntervalMs 须在 100..3600000"; return false;
    }
    if (c.qos != 0 && c.qos != 1) { err = "qos 仅支持 0 或 1"; return false; }
    return true;
}

// ════════════════════════════════════════════════════════════════════
// 变量别名加载期归一化
// ════════════════════════════════════════════════════════════════════
// variableAliases 段把用户自定义名映射到引擎内部契约名 (alias → internal).
// 归一化在此一次性完成, 而不是运行期到处查表 —— 因为最关键的消费点
// (TagGrouper::GetStartAddress / ResponseParser::Parse) 签名里没有 protocol,
// 拿不到 varAliasMap; 逐点传参会把名字解析散到整条调用链, 制造新的真源分裂。
//
// 改写范围:
//   协议: inputs / outputs (含 expr) · op.inputs / op.outputs (含 expr) ·
//         op.requestTemplate 占位符 · handshake[].requestTemplate 占位符
//   设备: variables 键
//   标签: variables / writeVariables 键
//
// 注: 管理面 ConfigStore 以 JSON 文本为操作边界 (不经本函数),
//     故 UI 上用户看到的仍是自己的自定义名。

/// 按 aliasMap 改写一段文本中的名字。
/// @param identifiers true = 同时改写裸标识符 (expr 用);
///                    false = 只改写 {Name:...} 占位符 (模板行用, 避免误伤 hex 字面量中的 A-F)
std::string RewriteAliasNames(const std::string& s,
                              const std::unordered_map<std::string, std::string>& am,
                              bool identifiers, bool& changed) {
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '{') {
            const std::size_t close = s.find('}', i + 1);
            if (close == std::string::npos) { out += s.substr(i); break; }
            const std::string inner = s.substr(i + 1, close - i - 1);
            const std::size_t colon = inner.find(':');
            std::string name = (colon == std::string::npos) ? inner : inner.substr(0, colon);
            std::unordered_map<std::string, std::string>::const_iterator it = am.find(name);
            if (it != am.end()) { name = it->second; changed = true; }
            out += '{';
            out += name;
            if (colon != std::string::npos) out += inner.substr(colon);
            out += '}';
            i = close + 1;
        } else if (identifiers
                   && (std::isalpha(static_cast<unsigned char>(c)) || c == '_')) {
            std::size_t j = i;
            while (j < s.size()
                   && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            const std::string ident = s.substr(i, j - i);
            std::unordered_map<std::string, std::string>::const_iterator it = am.find(ident);
            if (it != am.end()) { out += it->second; changed = true; }
            else out += ident;
            i = j;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

template <typename MapT>
MapT RenamedMap(const MapT& m,
                const std::unordered_map<std::string, std::string>& am) {
    MapT out;
    for (typename MapT::const_iterator it = m.begin(); it != m.end(); ++it) {
        std::unordered_map<std::string, std::string>::const_iterator a = am.find(it->first);
        out[(a != am.end()) ? a->second : it->first] = it->second;
    }
    return out;
}

/// 改写 VariableConfig 表: 键 + 每项的 expr
void RewriteVarConfigMap(std::unordered_map<std::string, Core::VariableConfig>& m,
                         const std::unordered_map<std::string, std::string>& am) {
    std::unordered_map<std::string, Core::VariableConfig> out;
    for (std::unordered_map<std::string, Core::VariableConfig>::const_iterator it = m.begin();
         it != m.end(); ++it) {
        Core::VariableConfig v = it->second;
        if (!v.expr.empty()) {
            bool ch = false;
            v.expr = RewriteAliasNames(v.expr, am, /*identifiers=*/true, ch);
        }
        std::unordered_map<std::string, std::string>::const_iterator a = am.find(it->first);
        out[(a != am.end()) ? a->second : it->first] = v;
    }
    m.swap(out);
}

/// 改写模板行序列 (只改 {Name:...} 占位符)
void RewriteTemplate(std::vector<std::string>& tpl,
                     const std::unordered_map<std::string, std::string>& am) {
    for (std::size_t i = 0; i < tpl.size(); ++i) {
        bool ch = false;
        tpl[i] = RewriteAliasNames(tpl[i], am, /*identifiers=*/false, ch);
    }
}

const Core::ProtocolConfig* FindProtocolByName(const LoadedConfig& cfg,
                                               const std::string& name) {
    for (std::size_t i = 0; i < cfg.protocols.size(); ++i) {
        if (cfg.protocols[i].protocolName == name) return &cfg.protocols[i];
    }
    return 0;
}

const Core::DeviceConfig* FindDeviceById(const LoadedConfig& cfg,
                                         const std::string& id) {
    for (std::size_t i = 0; i < cfg.root.devices.size(); ++i) {
        if (cfg.root.devices[i].id == id) return &cfg.root.devices[i];
    }
    return 0;
}

/// 归一化入口: 把三处配置里的别名统一改写成引擎内部名
void ApplyVariableAliases(LoadedConfig& cfg) {
    // ── 1. 协议侧 ──
    for (std::size_t i = 0; i < cfg.protocols.size(); ++i) {
        Core::ProtocolConfig& p = cfg.protocols[i];
        if (p.varAliasMap.empty()) continue;
        const std::unordered_map<std::string, std::string>& am = p.varAliasMap;

        RewriteVarConfigMap(p.inputs, am);
        RewriteVarConfigMap(p.outputs, am);
        for (std::unordered_map<std::string, Core::OperationConfig>::iterator op =
                 p.operations.begin(); op != p.operations.end(); ++op) {
            RewriteVarConfigMap(op->second.inputs, am);
            RewriteVarConfigMap(op->second.outputs, am);
            RewriteTemplate(op->second.requestTemplate, am);
        }
        for (std::size_t h = 0; h < p.handshake.size(); ++h) {
            RewriteTemplate(p.handshake[h].requestTemplate, am);
        }
    }

    // ── 2. 设备侧 (设备级变量缺省) ──
    for (std::size_t d = 0; d < cfg.root.devices.size(); ++d) {
        Core::DeviceConfig& dev = cfg.root.devices[d];
        const Core::ProtocolConfig* p = FindProtocolByName(cfg, dev.protocol);
        if (!p || p->varAliasMap.empty()) continue;
        dev.variables = RenamedMap(dev.variables, p->varAliasMap);
    }

    // ── 3. 标签侧 (tag.deviceId → device.protocol 定位别名表) ──
    for (std::size_t t = 0; t < cfg.root.tags.size(); ++t) {
        Core::TagDefinition& tag = cfg.root.tags[t];
        const Core::DeviceConfig* dev = FindDeviceById(cfg, tag.deviceId);
        if (!dev) continue;
        const Core::ProtocolConfig* p = FindProtocolByName(cfg, dev->protocol);
        if (!p || p->varAliasMap.empty()) continue;
        tag.variables = RenamedMap(tag.variables, p->varAliasMap);
        tag.writeVariables = RenamedMap(tag.writeVariables, p->varAliasMap);
    }
}

} // namespace

Core::Expected<LoadedConfig> ConfigDirectoryLoader::Load(
    const std::string& configDir, int supportedSchemaVersion) {

    LoadedConfig out;

    // ── 1. 协议文件集: 校验 + 解析一体 (单次 JSON 解析) ──
    const std::string protoDir = configDir + ConfigLayout::kProtocolsDirPath;
    const std::vector<std::string> protoFiles = ListJsonFiles(protoDir);

    for (size_t i = 0; i < protoFiles.size(); ++i) {
        const std::string path = protoDir + "/" + protoFiles[i];
        std::string text, err;
        if (!ReadTextFile(path, text, err)) {
            return Core::Unexpected(Core::Error::Code::ConfigError, err);
        }

        auto vp = ConfigValidator::validateAndParseProtocolJson(text, supportedSchemaVersion);
        if (!vp.has_value()) {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "协议文件校验失败 [" + path + "]",
                                    vp.error().message);
        }
        for (size_t w = 0; w < vp.value().result.warnings.size(); ++w) {
            std::cerr << "[WARN] " << path << ": " << vp.value().result.warnings[w]
                      << std::endl;
        }
        // errors 列表中断加载 (Fail-Fast: 协议语义错误吞掉仍启动 = 隐藏 bug)
        if (!vp.value().result.errors.empty()) {
            std::string detail;
            for (size_t e = 0; e < vp.value().result.errors.size(); ++e) {
                if (e) detail += "; ";
                detail += vp.value().result.errors[e];
            }
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "协议文件语义校验失败 [" + path + "]", detail);
        }
        out.protocols.push_back(vp.value().proto);
    }

    // ── 2. tags.json (设备 + 标签 + 全局韧性 + WebApi) ──
    // 协议集已在步骤 1 各自通过校验 — 根配置仅做根级规则 + 跨文件引用校验
    const std::string rootPath = configDir + ConfigLayout::kTagsFilePath;
    std::string rootText, err2;
    if (!ReadTextFile(rootPath, rootText, err2)) {
        return Core::Unexpected(Core::Error::Code::ConfigError, err2);
    }

    auto vr = ConfigValidator::validateAndParseConfigRootJson(
        rootText, out.protocols, supportedSchemaVersion);
    if (!vr.has_value()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根校验失败 [" + rootPath + "]",
                                vr.error().message);
    }
    for (size_t w = 0; w < vr.value().result.warnings.size(); ++w) {
        std::cerr << "[WARN] " << rootPath << ": " << vr.value().result.warnings[w]
                  << std::endl;
    }
    // errors 列表中断启动 (历史上仅 schemaVersion 门禁阻断, 语义错误吞掉仍启动;
    // 改为 errors 非空即整体加载失败, Fail-Fast 在加载期)
    if (!vr.value().result.errors.empty()) {
        std::string detail;
        for (size_t e = 0; e < vr.value().result.errors.size(); ++e) {
            if (e) detail += "; ";
            detail += vr.value().result.errors[e];
        }
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置根语义校验失败 [" + rootPath + "]", detail);
    }
    out.root = vr.value().root;

    // ── 3. server.json (全局服务端配置: 仿真 + 北向 MQTT 发布) ──
    // 可选缺失: 仿真 listenPort 默认 0 = 关闭, MQTT enabled 默认 false，对运行无副作用
    const std::string serverPath = configDir + ConfigLayout::kServerFilePath;
    std::string serverText, err3;
    if (ReadTextFile(serverPath, serverText, err3)) {
        try {
            nlohmann::json svDoc = detail::ParseConfigJsonText(serverText);
            if (!svDoc.is_object()) {
                return Core::Unexpected(Core::Error::Code::ConfigError,
                                        "server.json 顶层须为对象");
            }
            // schemaVersion
            if (svDoc.contains("schemaVersion") && svDoc["schemaVersion"].is_number_integer()) {
                out.server.schemaVersion = svDoc["schemaVersion"].get<int>();
                if (out.server.schemaVersion != supportedSchemaVersion) {
                    return Core::Unexpected(Core::Error::Code::ConfigError,
                        "server.json schemaVersion=" + std::to_string(out.server.schemaVersion) +
                        " 与配置根版本 " + std::to_string(supportedSchemaVersion) + " 不一致");
                }
            }
            // simulation 段 (可选; 旧版单实例回退，只在无协议级仿真时生效)
            if (svDoc.contains("simulation")) {
                std::string simErr;
                if (!detail::ParseSimulationDocImpl(
                        svDoc.at("simulation"), out.server.simulation, simErr)) {
                    return Core::Unexpected(Core::Error::Code::ConfigError,
                        "server.json simulation: " + simErr);
                }
                const std::vector<std::string> simErrs =
                    detail::ValidateSimulationConfig(out.server.simulation,
                                                     "server.json");
                if (!simErrs.empty()) {
                    std::string detail;
                    for (std::size_t e = 0; e < simErrs.size(); ++e) {
                        if (e) detail += "; ";
                        detail += simErrs[e];
                    }
                    return Core::Unexpected(Core::Error::Code::ConfigError,
                                            "server.json 仿真校验失败", detail);
                }
            }
            // mqtt 段 (可选; 北向发布, 缺省关闭)
            if (svDoc.contains("mqtt")) {
                std::string mqttErr;
                if (!ParseMqttDoc(svDoc.at("mqtt"), out.server.mqtt, mqttErr)) {
                    return Core::Unexpected(Core::Error::Code::ConfigError,
                                            "server.json mqtt: " + mqttErr);
                }
                if (!ValidateMqttConfig(out.server.mqtt, mqttErr)) {
                    return Core::Unexpected(Core::Error::Code::ConfigError,
                                            "server.json mqtt 校验失败", mqttErr);
                }
            }
        } catch (const std::exception& e) {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                "server.json 解析失败: " + std::string(e.what()));
        }
    }
    // server.json 缺失 = OK, 默认 server.simulation.listenPort=0 (关闭)

    // ── 4. 变量别名加载期归一化 (协议 + 设备 + 标签) ──
    //   必须在协议与根配置都解析完之后执行 — 标签的别名表来自其所属设备的协议.
    ApplyVariableAliases(out);

    return Core::Expected<LoadedConfig>(out);
}

}} // namespace MyProt::Service

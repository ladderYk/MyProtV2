// src/Service/src/ConfigStore.cpp — ConfigStore 骨架实现
// M1 骨架: 文件读写 / 校验 / 原子写 / 备份 / 回滚 / 热重载回调。
// 深度字段校验 (Config Schema §7) 与 POCO 序列化在后续阶段接入。

#include "MyProt/Service/ConfigStore.hpp"
#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Service/ConfigDirectoryLoader.hpp"   // ConfigLayout (磁盘布局契约名)

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <algorithm>

#include <windows.h>
#include <io.h>

namespace MyProt { namespace Service {

namespace {

// 窄字符路径 → 宽字符 (配置文件路径约定 UTF-8, 与 JSON 一致)
std::wstring ToWide(const std::string& s) {
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n) - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

} // namespace

ConfigStore::ConfigStore(const ConfigStoreOptions& options)
    : _opts(options) {}

// ── 名称校验: 仅允许无路径分隔符的合法文件名 (防路径穿越) ──
bool ConfigStore::IsValidName(const std::string& name) const {
    if (name.empty() || name == "." || name == "..") return false;
    if (name.find_first_of("/\\") != std::string::npos) return false;
    if (name.find_first_of(":*?\"<>|") != std::string::npos) return false;  // Windows 非法字符
    return true;
}

// ── 路径解析: 校验 scope+name 并规范为绝对安全路径 ──
std::string ConfigStore::ResolvePath(ConfigScope scope, const std::string& name,
                                     Core::Error* err) const {
    if (!IsValidName(name)) {
        if (err) *err = Core::Error::Make(Core::Error::Code::ConfigError,
                                          "非法配置名 (含路径分隔符或非法字符)", "name=" + name);
        return std::string();
    }
    std::string path = _opts.configDir;
    if (scope == ConfigScope::Protocol) {
        path += ConfigLayout::kProtocolsDirPath;
        path += "/";
        path += name;
        path += ".json";
    } else {  // Tags
        path += ConfigLayout::kTagsFilePath;
    }
    return path;
}

// ── 读文件 ──
Core::Expected<std::string> ConfigStore::ReadFile(const std::string& path) const {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "无法读取配置文件", "path=" + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return Core::Expected<std::string>(ss.str());
}

// ── 原子写: 临时文件 + MoveFileExW 原子替换 (Windows) ──
Core::Expected<void> ConfigStore::AtomicWrite(const std::string& path,
                                              const std::string& content) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out) {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "无法写入临时文件", "path=" + tmp);
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "写临时文件失败", "path=" + tmp);
        }
    }
    if (!::MoveFileExW(ToWide(tmp).c_str(), ToWide(path).c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(ToWide(tmp).c_str());
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "原子替换配置失败", "path=" + path);
    }
    return Core::VoidExpected();
}

// ── 备份当前版: 复制为 .bak.N (环形, 保留 _opts.backupRetention 份) ──
Core::Expected<void> ConfigStore::BackupFile(const std::string& path) {
    if (_opts.backupRetention <= 0) return Core::VoidExpected();

    std::ifstream src(path.c_str(), std::ios::binary);
    if (!src) return Core::VoidExpected();  // 首次保存无旧版, 跳过备份

    std::ostringstream ss;
    ss << src.rdbuf();
    const std::string content = ss.str();

    // 先清理超出保留上限的最老备份
    for (int i = _opts.backupRetention; i >= 1; --i) {
        std::string bak = path + ".bak." + std::to_string(i);
        if (::GetFileAttributesW(ToWide(bak).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        // 若到达上限仍需保留, 删除最老一份
        if (i == _opts.backupRetention) {
            ::DeleteFileW(ToWide(bak).c_str());
        }
    }
    // 轮转: bak.N ← bak.(N-1)
    for (int i = _opts.backupRetention - 1; i >= 1; --i) {
        std::string from = path + ".bak." + std::to_string(i);
        std::string to   = path + ".bak." + std::to_string(i + 1);
        if (::GetFileAttributesW(ToWide(from).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ::MoveFileExW(ToWide(from).c_str(), ToWide(to).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
    }
    // 当前版 → bak.1
    std::string bak1 = path + ".bak.1";
    std::ofstream out(bak1.c_str(), std::ios::binary | std::ios::trunc);
    if (!out) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "创建备份失败", "path=" + bak1);
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.flush();
    return out ? Core::VoidExpected()
               : Core::Unexpected(Core::Error::Code::ConfigError,
                                  "写备份失败", "path=" + bak1);
}

// ── 触发热重载 ──
Core::Expected<void> ConfigStore::TriggerReload() {
    if (!_reload) return Core::VoidExpected();  // 未注册回调 → 仅落盘
    return _reload();
}

// ── List ──
Core::Expected<std::vector<std::string>> ConfigStore::List(ConfigScope scope) const {
    if (scope == ConfigScope::Tags) {
        return Core::Expected<std::vector<std::string>>(
            std::vector<std::string>(1, "tags"));
    }
    // Protocol: 枚举 configDir/protocols/*.json
    std::vector<std::string> names;
    std::string dir = _opts.configDir + ConfigLayout::kProtocolsDirPath + "/";
    std::string pattern = dir + "*.json";
    _finddata_t fd;
    intptr_t h = ::_findfirst(pattern.c_str(), &fd);
    if (h != -1) {
        do {
            std::string file(fd.name);
            if (file.size() > 5 && file.substr(file.size() - 5) == ".json") {
                names.push_back(file.substr(0, file.size() - 5));
            }
        } while (::_findnext(h, &fd) == 0);
        ::_findclose(h);
    }
    std::sort(names.begin(), names.end());
    return Core::Expected<std::vector<std::string>>(names);
}

// ── Get ──
Core::Expected<std::string> ConfigStore::Get(ConfigScope scope, const std::string& name) const {
    Core::Error err;
    std::string path = ResolvePath(scope, name, &err);
    if (path.empty()) return Core::UnexpectedType{err};
    return ReadFile(path);
}

// ── Validate (JSON 语法 + schemaVersion 门禁 + 深度字段校验) ──
Core::Expected<std::vector<std::string>> ConfigStore::Validate(
    ConfigScope scope, const std::string& name, const std::string& payload) const {

    std::vector<std::string> errors;
    (void)scope; (void)name;

    // schemaVersion 版本门禁 + 深度字段校验 (Config Schema §7)
    if (scope == ConfigScope::Protocol) {
        auto deepResult = ConfigValidator::validateProtocolJson(payload, _opts.supportedSchemaVersion);
        if (!deepResult.has_value()) {
            errors.push_back(deepResult.error().message);
            return Core::Expected<std::vector<std::string>>(errors);
        }
        const ValidationResult& vr = deepResult.value();
        for (const auto& e : vr.errors)   errors.push_back(e);
        for (const auto& w : vr.warnings) errors.push_back("[WARN] " + w);

        // ADR-0012 §2: 保存期试算校验 — 深度校验通过后, 对含 {Name:raw} 的写操作
        // 真实渲染一帧并核对成帧长度槽位; expr 求值失败/帧长不一致 → 阻断保存
        auto poco = ConfigValidator::parseProtocolJson(payload);
        if (poco.has_value()) {
            std::vector<std::string> frameErrs =
                ConfigValidator::CheckFrameConsistency(poco.value());
            errors.insert(errors.end(), frameErrs.begin(), frameErrs.end());
        }
    } else {
        // 跨文件校验需协议全文: 从磁盘枚举当前协议集传入,
        // 否则空列表下"设备引用的协议不存在"规则会误拦一切 tags 保存
        std::vector<std::string> protoJsons;
        const Core::Expected<std::vector<std::string> > names =
            List(ConfigScope::Protocol);
        if (names.has_value()) {
            for (size_t i = 0; i < names.value().size(); ++i) {
                Core::Expected<std::string> pj =
                    Get(ConfigScope::Protocol, names.value()[i]);
                if (pj.has_value()) protoJsons.push_back(pj.value());
            }
        }
        auto deepResult = ConfigValidator::validateConfigRootJson(
            payload, protoJsons, _opts.supportedSchemaVersion);
        if (!deepResult.has_value()) {
            errors.push_back(deepResult.error().message);
            return Core::Expected<std::vector<std::string>>(errors);
        }
        const ValidationResult& vr = deepResult.value();
        for (const auto& e : vr.errors)   errors.push_back(e);
        for (const auto& w : vr.warnings) errors.push_back("[WARN] " + w);
    }

    return Core::Expected<std::vector<std::string>>(errors);
}

// ── Save: 校验 → 备份 → 原子写 → 热重载 (失败回滚) ──
Core::Expected<void> ConfigStore::Save(ConfigScope scope, const std::string& name,
                                       const std::string& payload) {
    std::lock_guard<std::mutex> lock(_mutex);

    Core::Error err;
    std::string path = ResolvePath(scope, name, &err);
    if (path.empty()) return Core::UnexpectedType{err};

    // 1. 校验 (Warning 放行: 仅非 "[WARN] " 前缀条目阻断保存)
    auto validation = Validate(scope, name, payload);
    if (!validation.has_value()) return Core::UnexpectedType{validation.error()};
    // 汇总**全部**阻断项一并回报 (编号换行分隔), 管理面按行渲染为清单.
        //   不能只回第一条 → 否则作者陷入"改一条、存一次、看下一条"的串行试错.
    std::vector<std::string> blocking;
    {
        const std::vector<std::string>& items = validation.value();
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].compare(0, 7, "[WARN] ") != 0) blocking.push_back(items[i]);
        }
    }
    if (!blocking.empty()) {
        std::string detail = "配置校验失败, 共 " + std::to_string(blocking.size())
                             + " 项:";
        for (size_t i = 0; i < blocking.size(); ++i) {
            detail += "\n" + std::to_string(i + 1) + ") " + blocking[i];
        }
        return Core::Unexpected(Core::Error::Code::ConfigError, detail, name);
    }

    // 2. 备份当前版
    auto backup = BackupFile(path);
    if (!backup.has_value()) return Core::UnexpectedType{backup.error()};

    // 3. 原子写
    auto write = AtomicWrite(path, payload);
    if (!write.has_value()) return Core::UnexpectedType{write.error()};

    // 4. 热重载; 失败自动回滚到 bak.1 (已持锁, 走无锁内部实现)
    auto reload = TriggerReload();
    if (!reload.has_value()) {
        Core::Expected<void> rollback = RollbackUnlocked(scope, name, "bak.1");
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "配置已保存但热重载失败, 已回滚: " + reload.error().message
                                + (rollback.has_value() ? std::string() : std::string(" (回滚也失败)")));
    }
    return Core::VoidExpected();
}

// ── ListBackups ──
Core::Expected<std::vector<std::string>> ConfigStore::ListBackups(
    ConfigScope scope, const std::string& name) const {

    Core::Error err;
    std::string path = ResolvePath(scope, name, &err);
    if (path.empty()) return Core::UnexpectedType{err};

    std::vector<std::string> tags;
    for (int i = 1; i <= _opts.backupRetention; ++i) {
        std::string bak = path + ".bak." + std::to_string(i);
        if (::GetFileAttributesW(ToWide(bak).c_str()) != INVALID_FILE_ATTRIBUTES) {
            tags.push_back("bak." + std::to_string(i));
        }
    }
    return Core::Expected<std::vector<std::string>>(tags);
}

// ── Rollback: 从备份恢复到主文件 (加锁外壳) ──
Core::Expected<void> ConfigStore::Rollback(ConfigScope scope, const std::string& name,
                                           const std::string& backupTag) {
    std::lock_guard<std::mutex> lock(_mutex);
    return RollbackUnlocked(scope, name, backupTag);
}

// ── Rollback 内部实现 (调用方须已持有 _mutex; Save 自动回滚路径复用) ──
Core::Expected<void> ConfigStore::RollbackUnlocked(ConfigScope scope,
                                                   const std::string& name,
                                                   const std::string& backupTag) {
    Core::Error err;
    std::string path = ResolvePath(scope, name, &err);
    if (path.empty()) return Core::UnexpectedType{err};
    // 备份标签严格限定 "bak.<正整数>" — backupTag 未经 IsValidName 过滤, 否则
    // "bak.1\\..\\..\\win.ini" 可借 Windows 路径词法归一化穿越读取任意文件。
    if (backupTag.size() <= 4 || backupTag.compare(0, 4, "bak.") != 0) {
        return Core::Unexpected(Core::Error::Code::ConfigError, "非法备份标签", "tag=" + backupTag);
    }
    for (std::size_t i = 4; i < backupTag.size(); ++i) {
        if (backupTag[i] < '0' || backupTag[i] > '9') {
            return Core::Unexpected(Core::Error::Code::ConfigError,
                                    "非法备份标签 (仅 bak.<数字>)", "tag=" + backupTag);
        }
    }

    std::string bak = path + "." + backupTag;
    auto content = ReadFile(bak);
    if (!content.has_value()) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "备份不存在", "path=" + bak);
    }
    auto write = AtomicWrite(path, content.value());
    if (!write.has_value()) return Core::UnexpectedType{write.error()};
    return TriggerReload();
}

// ── Delete: 删除配置文件及全部备份 (仅 Protocol) ──
Core::Expected<void> ConfigStore::Delete(ConfigScope scope, const std::string& name) {
    std::lock_guard<std::mutex> lock(_mutex);

    if (scope == ConfigScope::Tags) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "tags.json 为固定配置文件, 不支持删除");
    }

    Core::Error err;
    std::string path = ResolvePath(scope, name, &err);
    if (path.empty()) return Core::UnexpectedType{err};

    if (::GetFileAttributesW(ToWide(path).c_str()) == INVALID_FILE_ATTRIBUTES) {
        return Core::Unexpected(Core::Error::Code::ProtocolNotFound,
                                "协议配置不存在", "name=" + name);
    }
    if (!::DeleteFileW(ToWide(path).c_str())) {
        return Core::Unexpected(Core::Error::Code::ConfigError,
                                "删除配置文件失败", "path=" + path);
    }
    // 连带清理备份
    for (int i = 1; i <= _opts.backupRetention; ++i) {
        std::string bak = path + ".bak." + std::to_string(i);
        if (::GetFileAttributesW(ToWide(bak).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ::DeleteFileW(ToWide(bak).c_str());
        }
    }
    return Core::VoidExpected();
}

// ── 依赖注入 ──
void ConfigStore::SetReloadHandler(ReloadHandler handler) {
    std::lock_guard<std::mutex> lock(_mutex);
    _reload = handler;
}

// ── 手动热重载 ──
Core::Expected<void> ConfigStore::Reload() {
    std::lock_guard<std::mutex> lock(_mutex);
    return TriggerReload();
}

// ────────── ConfigValidator JSON 入口 ──────────
// M2: 深度十五项规则校验与跨文件引用校验已移入 ConfigDeepValidator.cpp 独立实现。

}} // namespace MyProt::Service
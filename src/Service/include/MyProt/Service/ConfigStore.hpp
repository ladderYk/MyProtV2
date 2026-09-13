// src/Service/include/MyProt/Service/ConfigStore.hpp
// ConfigStore — 配置读写服务 (配置界面 M1 存储后端, 见 design-proposal/v2-config-ui-design)
// 职责: JSON 配置文件的读取 / 校验 / 原子写 / 备份 / 回滚, 保存后触发热重载回调。
// 约束: 纯 C++11 / VS2015 (ADR-0010 §3); 错误处理统一走 Core::Expected。

#pragma once
#include <string>
#include <vector>
#include <functional>
#include <mutex>

#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Config.hpp"   // kSupportedSchemaVersion

namespace MyProt { namespace Service {

/// 配置作用域 — 对应配置文件布局:
///   configDir/protocols/{name}.json   (协议级, ConfigScope::Protocol)
///   configDir/tags.json               (设备+标签+韧性+管理面, ConfigScope::Tags)
enum class ConfigScope { Protocol, Tags };

/// ConfigStore 构造选项
struct ConfigStoreOptions {
    std::string configDir;          // 配置文件根目录 (含 protocols/ 与 tags.json)
    int         supportedSchemaVersion;  // 当前支持的配置代际 (ADR-0005 版本门禁)
    int         backupRetention;    // 保留的备份份数 (环形)

    ConfigStoreOptions()
        : configDir("configs"), supportedSchemaVersion(Core::kSupportedSchemaVersion)
        , backupRetention(3) {}
};

/// 配置读写服务 — 配置界面 M1 的存储后端。
/// 完整链路: 读 / 校验 / 原子写 / 备份 / 回滚。
/// 校验经 ConfigValidator JSON 入口执行 Config_Schema §7 深度规则;
/// 本类以 JSON 文本为操作边界, 不做 POCO 转换 (解析归 ConfigDirectoryLoader)。
class ConfigStore {
public:
    /// 热重载回调 — 由 ConfigReloadService 注册; Save 成功后调用。
    using ReloadHandler = std::function<Core::VoidExpected()>;

    explicit ConfigStore(const ConfigStoreOptions& options);

    // ── 读 ──────────────────────────────────────────────
    /// 列出某 scope 下所有配置项名称 (Protocol → 协议文件名; Tags → ["tags"])。
    Core::Expected<std::vector<std::string>> List(ConfigScope scope) const;

    /// 读取某配置项, 返回原始 JSON 文本。
    Core::Expected<std::string> Get(ConfigScope scope, const std::string& name) const;

    // ── 校验 ────────────────────────────────────────────
    /// 校验配置: JSON 语法 + schemaVersion 门禁 + Config_Schema §7 深度字段校验
    /// (Protocol → validateProtocolJson; Tags → validateConfigRootJson,
    ///  并附当前 protocols/*.json 全文做跨文件引用校验)。
    /// 返回全部错误清单 (Warning 带 "[WARN] " 前缀); 空 = 通过。
    Core::Expected<std::vector<std::string>> Validate(ConfigScope scope,
                                                      const std::string& name,
                                                      const std::string& payload) const;

    // ── 写 ──────────────────────────────────────────────
    /// 保存配置: 校验 → 备份当前版 → 原子写 → 触发热重载。
    /// 校验失败不落盘; reload 失败自动回滚并返回错误。
    Core::Expected<void> Save(ConfigScope scope, const std::string& name, const std::string& payload);

    // ── 备份 / 回滚 ─────────────────────────────────────
    /// 列出某配置项可回滚的备份标签 (e.g. "bak.1", "bak.2")。
    Core::Expected<std::vector<std::string>> ListBackups(ConfigScope scope,
                                                         const std::string& name) const;

    /// 回滚到指定备份标签。
    Core::Expected<void> Rollback(ConfigScope scope, const std::string& name,
                                  const std::string& backupTag);

    // ── 删除 ────────────────────────────────────────────
    /// 删除配置项及其全部备份 (仅 Protocol; Tags 为固定单文件不支持删除)。
    Core::Expected<void> Delete(ConfigScope scope, const std::string& name);

    // ── 依赖注入 ────────────────────────────────────────
    /// 注册热重载回调 (由 ConfigReloadService 在装配期注入)。
    void SetReloadHandler(ReloadHandler handler);

    /// 手动触发热重载 (WebApi /api/config/reload 端点); 未注册回调时直接成功。
    Core::Expected<void> Reload();

    /// 当前支持的配置代际 (ADR-0005); GET /api/config/schema 端点下发用。
    int SupportedSchemaVersion() const { return _opts.supportedSchemaVersion; }

private:
    // 解析并校验配置项路径; 防路径穿越 (仅允许合法文件名)。
    std::string ResolvePath(ConfigScope scope, const std::string& name,
                            Core::Error* err) const;
    bool IsValidName(const std::string& name) const;

    Core::Expected<std::string> ReadFile(const std::string& path) const;
    Core::Expected<void> AtomicWrite(const std::string& path, const std::string& content);
    Core::Expected<void> BackupFile(const std::string& path);
    Core::Expected<void> TriggerReload();

    /// Rollback 无锁内部实现 — 调用方须已持有 _mutex;
    /// Save 的自动回滚路径复用 (公共 Rollback 加锁后转调, 避免同线程重入死锁)。
    Core::Expected<void> RollbackUnlocked(ConfigScope scope, const std::string& name,
                                          const std::string& backupTag);

    ConfigStoreOptions _opts;
    ReloadHandler _reload;      // 未注册时为空, Save 跳过 reload
    mutable std::mutex _mutex;  // 序列化写操作, 避免与 reload 竞争
};

}} // namespace MyProt::Service
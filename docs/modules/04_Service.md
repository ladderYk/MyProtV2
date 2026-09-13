# MyProtV2 — Service 模块

> **所属**: MyProtV2 模块设计系列
> **上一节**: [Transport 模块](./03_Transport.md)
> **下一节**: [Gateway 模块](./05_Gateway.md)

---

**依赖**: `Core`（nlohmann/json 仅在实现文件内部使用，头文件不外泄）

> ⚠️ 配置契约（字段、文法、校验规则）以 [Config_Schema.md](../Config_Schema.md) 为准。
> 本模块**没有接口继承体系**——全部为具体类与静态方法，错误统一用 `Core::Expected`
> （纯 C++11 / VS2015，ADR-0010）。

## 4.0 组件总览

```
src/Service/
├── include/MyProt/Service/
│   ├── ConfigValidator.hpp        # 深度校验器 (声明)
│   ├── ConfigDirectoryLoader.hpp  # 目录布局加载器
│   ├── ConfigStore.hpp            # 配置读写服务
│   ├── SchemaRegistry.hpp         # 字段描述符注册表
│   └── SessionContext.hpp         # 设备会话上下文
└── src/
    ├── ConfigDeepValidator.cpp    # ConfigValidator 实现 (~1450 行)
    ├── ConfigDirectoryLoader.cpp
    ├── ConfigStore.cpp
    ├── SchemaRegistry.cpp
    └── SessionContext.cpp
```

| 组件 | 职责 | 主要消费者 |
|---|---|---|
| `ConfigValidator` | 十五项规则深度校验 + JSON→POCO 解析（一体入口） | `ConfigDirectoryLoader` / `ConfigStore` |
| `ConfigDirectoryLoader` | 目录布局加载（生产启动链路） | App `RunProduction` |
| `ConfigStore` | 读写、原子写、备份/回滚 + 热重载回调 | `WebApiServer` |
| `SchemaRegistry` | 字段描述符注册表 — `GET /api/config/schema` | `WebApiServer` |
| `SessionContext` | 会话变量 + 断路器韧性策略 | Gateway `ChannelManager` |

> **历史注记**: 早期设计稿中的 `IProtocolRepository` / `InMemoryProtocolRepository` /
> `IConfigLoader` / `JsonConfigLoader` / `FileWatcher` / `ConfigReloadService`
> 均未实现，相关章节已删除。
> - `FileWatcher` (inotify/efsw 形态) 已**撤回**（v1 范围外，配置热重载走 `ConfigStore::SetReloadHandler` 回调 + WebApi `PUT /api/config` / `POST /api/config/reload` 手动触发）；
> - `ConfigReloadService` 早期命名被 `ConfigStore::SetReloadHandler` 取代。
> 协议查找由 App 层闭包经 `Gateway::ProtocolLookup`
> 函数别名注入（见 [05_Gateway.md](./05_Gateway.md) §5.1）；热重载由
> `ConfigStore::SetReloadHandler` 回调驱动（见 [08_App.md](./08_App.md)）。

---

## 4.1 ConfigValidator

```cpp
// src/Service/include/MyProt/Service/ConfigValidator.hpp

namespace MyProt { namespace Service {

/// 校验结果: 错误 (阻止启动) 与警告 (不阻断, 日志输出)
struct ValidationResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool hasErrors() const;
    void addError(const std::string& msg);
    void addWarning(const std::string& msg);
    void merge(const ValidationResult& other);
};

/// 校验 + 解析一体产出: 单次 JSON 解析同时得到深度校验结果与 POCO
struct ProtocolValidationOutcome {
    ValidationResult result;
    Core::ProtocolConfig proto;
};

struct ConfigRootValidationOutcome {
    ValidationResult result;
    Core::ConfigRoot root;
};

class ConfigValidator {
public:
    explicit ConfigValidator(int supportedSchemaVersion = 1);

    // ── POCO 入口 ──
    ValidationResult validateProtocol(const Core::ProtocolConfig& proto) const;
    ValidationResult validateConfigRoot(
        const Core::ConfigRoot& root,
        const std::vector<Core::ProtocolConfig>& protocols =
            std::vector<Core::ProtocolConfig>()) const;

    // ── JSON 文本入口 (供 ConfigStore 直接调用) ──
    static Core::Expected<ValidationResult> validateProtocolJson(
        const std::string& json, int supportedSchemaVersion = 1);
    static Core::Expected<ValidationResult> validateConfigRootJson(
        const std::string& json,
        const std::vector<std::string>& protocolJsons = std::vector<std::string>(),
        int supportedSchemaVersion = 1);

    // ── JSON → POCO 解析 (仅解析, 不深度校验) ──
    static Core::Expected<Core::ProtocolConfig> parseProtocolJson(const std::string& json);
    static Core::Expected<Core::ConfigRoot> parseConfigRootJson(const std::string& json);

    // ── 校验 + 解析一体入口 (单次 JSON 解析; 生产加载链路) ──
    static Core::Expected<ProtocolValidationOutcome> validateAndParseProtocolJson(
        const std::string& json, int supportedSchemaVersion = 1);
    static Core::Expected<ConfigRootValidationOutcome> validateAndParseConfigRootJson(
        const std::string& json,
        const std::vector<Core::ProtocolConfig>& protocols,
        int supportedSchemaVersion = 1);
};

}} // namespace MyProt { namespace Service
```

### 校验范围

私有方法与 Config_Schema 规则逐组对应，全部**收集式**（不提前终止），唯一例外：

| 分组 | 方法 | 规则 |
|---|---|---|
| 版本门禁 | `checkVersionGate` | **唯一提前终止**: schemaVersion 不在支持代际立即返回（ADR-0005） |
| 协议层 | `validateTransport` / `validateFraming` / `validateOperations` / `validateHandshake` | §7 规则 1~8 |
| 模板文法 | `validateTemplate` / `validateBuiltInFunctions` / `validateChecksumWidths` | §3 占位符文法 + 内建函数 + 校验和宽度匹配（`validateChecksumWidths` 仅做 `line.width` 字段与算法名 `crc16/crc32/...` 的字符串匹配，不调用已撤回 A1 的 `ComputeChecksum`/`ChecksumSize` 函数） |
| 设备层 | `validateDevice` | §7 规则 9~11 |
| 标签层 | `validateTag` | §7 规则 12~15（含跨文件引用） |
| 韧性 | `validateResilience` | §11 |
| 管理面 | `validateWebApi` | §12 |

### 三类入口的选用

- **生产加载**（`ConfigDirectoryLoader`）→ `validateAndParse*`：一次 JSON 解析同时产出
  校验结果与 POCO，消除历史上"先 validate 再 parse"的双重解析；
- **管理面保存**（`ConfigStore::Validate/Save`）→ `validate*Json`：只需判定合法性与错误清单，不需要 POCO；
- **根配置阶段**：`validateAndParseConfigRootJson` 接收**已解析的协议集**（而非协议 JSON 文本），
  只执行根级规则与跨文件引用校验，不重复逐协议解析——协议侧工作在协议加载阶段完成一次即可。

---

## 4.2 ConfigDirectoryLoader

```cpp
// src/Service/include/MyProt/Service/ConfigDirectoryLoader.hpp

namespace MyProt { namespace Service {

/// 目录加载产物: 协议集 + 配置根 (均已通过深度校验与解析)
struct LoadedConfig {
    std::vector<Core::ProtocolConfig> protocols;
    Core::ConfigRoot root;
};

class ConfigDirectoryLoader {
public:
    /// @param configDir              配置目录
    /// @param supportedSchemaVersion 版本门禁代际 (ADR-0005)
    static Core::Expected<LoadedConfig> Load(const std::string& configDir,
                                             int supportedSchemaVersion);
};

}} // namespace MyProt { namespace Service
```

### 加载流程（单次解析链路）

目录布局（Config_Schema §1）：

```
<dir>/
├── protocols/*.json    # 一文件一协议
├── tags.json           # 设备 + 标签 + 全局韧性 + WebApi
└── server.json         # 全局服务端配置（仿真 + 未来 alertSink/webhook）；可选缺失
```

1. 遍历 `protocols/*.json`，每个文件调用 `validateAndParseProtocolJson` —
   深度校验（Fail-Fast，错误携带文件名）与 POCO 转换一次完成；
2. 任一协议文件校验失败 → 整体 `Load` 失败；
3. 读取 `tags.json`，调用 `validateAndParseConfigRootJson(text, protocols, ver)`
   — 复用第 1 步已解析的协议集做跨文件引用校验，**不再重复解析协议**；
4. 读取 `server.json`（缺失 = OK，`ServerConfig.simulation.listenPort` 默认 0 即关闭仿真），
   解析后挂到 `LoadedConfig.server` 供 `RuntimeGlue` 启动仿真器；解析失败 = 整体 `Load` 失败；
5. 校验期 Warning 输出到 stderr，不阻断加载。

> 演进记录: 改造前协议文件经历 3 次 JSON 解析、tags.json 经历 2 次；
> 收敛后每文件严格 1 次（v4.1 重构 Item 1）。

---

## 4.3 ConfigStore

配置读写服务——WebApi 管理面（配置界面 M1）的存储后端。

```cpp
// src/Service/include/MyProt/Service/ConfigStore.hpp

namespace MyProt { namespace Service {

/// 配置作用域 — 对应文件布局:
///   configDir/protocols/{name}.json   (ConfigScope::Protocol)
///   configDir/tags.json               (ConfigScope::Tags, 固定单文件)
enum class ConfigScope { Protocol, Tags };

struct ConfigStoreOptions {
    std::string configDir;                 // 默认 "configs"
    int         supportedSchemaVersion;    // 默认 2 (ADR-0005 版本门禁)
    int         backupRetention;           // 默认 3 (环形备份份数)
};

class ConfigStore {
public:
    /// 热重载回调 — 由宿主 (App RuntimeGlue) 注册; Save 成功后调用
    using ReloadHandler = std::function<Core::VoidExpected()>;

    explicit ConfigStore(const ConfigStoreOptions& options);

    // ── 读 ──
    Core::Expected<std::vector<std::string>> List(ConfigScope scope) const;
    Core::Expected<std::string> Get(ConfigScope scope, const std::string& name) const;

    // ── 校验: JSON 语法 + schemaVersion 门禁 + Config_Schema §7 深度规则 ──
    // 返回全部错误清单; 空 = 通过
    Core::Expected<std::vector<std::string>> Validate(ConfigScope scope,
                                                      const std::string& name,
                                                      const std::string& payload) const;

    // ── 写: 校验 + 备份当前值 + 原子写 + 触发热重载 ──
    // 校验失败不落盘; reload 失败自动回滚并返回错误
    Core::Expected<void> Save(ConfigScope scope, const std::string& name,
                              const std::string& payload);

    // ── 备份 / 回滚 ──
    Core::Expected<std::vector<std::string>> ListBackups(ConfigScope scope,
                                                         const std::string& name) const;
    Core::Expected<void> Rollback(ConfigScope scope, const std::string& name,
                                  const std::string& backupTag);

    // ── 删除 (仅 Protocol; Tags 固定单文件不支持) ──
    Core::Expected<void> Delete(ConfigScope scope, const std::string& name);

    // ── 依赖注入 / 手动重载 ──
    void SetReloadHandler(ReloadHandler handler);
    Core::Expected<void> Reload();                  // /api/config/reload 端点
    int SupportedSchemaVersion() const;             // GET /api/config/schema 下发
};

}} // namespace MyProt { namespace Service
```

### 关键实现约定

- **深度校验已接入**：`Validate` 对 Protocol 走 `validateProtocolJson`、对 Tags 走
  `validateConfigRootJson`（附协议文本做跨文件引用），不再是早期骨架的纯语法检查；
- **原子写**：临时文件 + `MoveFileEx(MOVEFILE_REPLACE_EXISTING)` 原子替换（恒为原子写）；
- **环形备份**：每次 Save 前把当前版本存为 `bak.N`，超过 `backupRetention` 淘汰最旧；
- **自动回滚**：Save 后触发热重载，若 reload 回调报错（如新配置非法）则
  `RollbackUnlocked` 回退到前一版本——复用内部无锁实现，公共 `Rollback` 加锁转调，
  避免同线程重入死锁；
- **路径安全**：`ResolvePath` 拒绝路径穿越（仅允许合法文件名）；`Rollback` 的 `backupTag` 严格限定 `bak.<正整数>`，防借 Windows 路径词法归一化越界读取；
- `_mutex` 序列化写操作，避免写与 reload 竞争。

---

## 4.4 SchemaRegistry

`GET /api/config/schema` 数据源，供 Vue 管理界面动态表单渲染；
表内容与 docs/Config_Schema.md 契约手工保持一致。

```cpp
// src/Service/include/MyProt/Service/SchemaRegistry.hpp

namespace MyProt { namespace Service {

/// 字段描述符 — 描述配置 JSON 中单个字段的结构约束
struct FieldDescriptor {
    std::string name;                        // 字段名
    std::string type;                        // string/int/uint8/.../enum/object/array/map
    bool required;
    std::string defaultValue;                // 空 = 无缺省说明
    std::string description;                 // 语义说明 (含校验边界提示)
    std::vector<std::string> enumValues;     // type == "enum" 时有效
    std::vector<FieldDescriptor> children;   // object / array<object> 时有效
};

class SchemaRegistry {
public:
    static const std::vector<FieldDescriptor>& RootFields();     // schemaVersion/resilience/webApi/devices/tags
    static const std::vector<FieldDescriptor>& ProtocolFields(); // transport/framing/operations/handshake/builtInFunctions...
    static const std::vector<FieldDescriptor>& DeviceFields();
    static const std::vector<FieldDescriptor>& TagFields();

    /// 序列化整个注册表:
    /// {"schemaVersion":N,"sections":{"root":[...],"protocol":[...],"device":[...],"tag":[...]}}
    static std::string ToJson(int supportedSchemaVersion);
};

}} // namespace MyProt { namespace Service
```

> 已知架构债: 该表与 `ConfigValidator` 的规则各自独立维护，存在双源漂移风险
> （收敛方案见剩余工作清单 B4）。

---

## 4.5 SessionContext

设备会话上下文——Gateway `ChannelManager` 为每台设备持有一个（经 `ChannelManager::GetSession(deviceId)` 获取，见 05_Gateway.md §5.2）。

```cpp
// src/Service/include/MyProt/Service/SessionContext.hpp

namespace MyProt { namespace Service {

/// 断路器状态
enum class CircuitState { Closed, Open, HalfOpen };

class SessionContext {
public:
    explicit SessionContext(const Core::DeviceConfig& config);

    // ── 握手会话变量 ──
    Core::Optional<std::string> GetSessionVar(const std::string& name) const;
    void SetSessionVar(const std::string& name, const std::string& value);

    // ── 断路器韧性 (参数取自 DeviceConfig.resilience) ──
    void RecordFailure();          // 连续失败累计, 达阈值则 Open
    void RecordSuccess();          // 成功复位
    bool CanProceed() const;       // Open 且冷却到期则惰性转 HalfOpen 放行探测
    CircuitState GetCircuitState() const;

private:
    Core::DeviceConfig _config;
    std::unordered_map<std::string, std::string> _sessionVars;
    mutable std::mutex _mutex;

    Core::ResilienceConfig _resilience;
    mutable CircuitState _circuitState;   // const 下惰性转换所需
    int _consecutiveFailures;
    std::chrono::steady_clock::time_point _openedAt;  // 熔断打开时刻 (冷却计时起点)
    mutable int _halfOpenProbes;
};

}} // namespace MyProt { namespace Service
```

### 断路器状态机

```
            连续失败 ≥ 阈值                  冷却到期 (CanProceed 惰性判定)
Closed ───────────────────────► Open ────────────────────────► HalfOpen
   ▲                                                         │
   │                                                         │
   └──────────────── 探测成功 (RecordSuccess) ◄───────────────┘
                                  │
                                  └── 探测失败 → 回到 Open
```

- 冷却计时起点为 `_openedAt`（进入 Open 时刻）；
- `CanProceed()` 声明为 `const`，Open→HalfOpen 的惰性转换通过 `mutable` 字段完成；
- 所有状态访问经 `_mutex` 保护（`GetSessionVar`/`GetCircuitState` 声明 const，故 mutex 为 mutable）。

---

## 4.6 设备生命周期状态机

断路器是"流量层瞬态许可——决定请求是否放行"。设备生命周期是"连接层持久维度——决定设备当前是否能用、正在做什么"。两者正交，可同时存在。
```cpp
// src/Service/include/MyProt/Service/DeviceLifecycle.hpp
namespace MyProt { namespace Service {

enum class DeviceLifecycleState {
    New = 0,        // 已注册, 从未连接
    Connecting = 1, // 物理连接/握手中
    Connected = 2,  // 物理通道+握手均已建立, 业务可用
    Degraded = 3,   // 通道死了或正在重连, 未到永久禁用门槛
    Disabled = 4    // 永久禁用 (协议/握手 BuildError 等不可恢复错误)
};

// SessionContext 增设:
void SetLifecycle(DeviceLifecycleState s);            // 幂等: 无变化不计转换
DeviceLifecycleState GetLifecycle() const;
}}
```

### 状态机迁移表
```
New ──(GOC)──► Connecting ──(握手+连接成功)──► Connected
                │                                │
                │                                │ (连接级故障/通道死)
                │                                ▼
                └─(连接/握手失败)──► Degraded ◄───┘
                                            │
                          (下一轮 GOC)──────┤
                                            │
                          (重连成功)────────► Connected

* ──► Disabled   协议/握手 BuildError 等不可恢复错误
  Disabled ──► Connecting  需配置热重载恢复 (本轮未启用 enabled 字段,
                              ResetDevices 重建通道即可自然重连)
```

### 触发者表

| 迁移 | 触发者 |
|---|---|
| New → Connecting | `ChannelManager::GetOrCreateChannel` 入口（带 `connecting=true`） |
| Connecting → Connected | `PerformConnect` OK + 握手序列完成 |
| Connecting → Degraded | `PerformConnect` 失败 / `PerformHandshake` 失败 / 握手模板 `BuildError` |
| Connected → Degraded | `PollingEngine` GOC 失败 / 预算耗尽 / 重试耗尽且属连接级错误 |
| Degraded → Connecting | 下一轮 timer 触发 GOC |
| Degraded → Connected | GOC 成功完成 |
| * → Disabled | 协议/握手 `BuildError`（待后续"重复 N 次累积策略"启用） |

### 故障分类（`Core::IsLifecycleDegrading`）
| 错误码 | 类别 | 是否触发 Degraded |
|---|---|---|
| `Timeout` / `ConnectionRefused` / `ConnectionClosed` / `Busy` | 连接层 | ✅ |
| `InvalidResponse` / `ParseError` / `BuildError` / `TypeConversionError` | 协议层 | ❌ |
| `TagNotFound` / `ProtocolNotFound` / `DeviceNotFound` | 配置层 | ❌ |
| `CircuitOpen` | 流量层 | ❌（熔断器已在管） |
| `WriteTimeout` / `WriteFailed` | 写操作 | ❌ |
| `ConfigError` / `InternalError` | 启动/内部 | InternalError ✅（通道工厂未注册, 创建失败视作连接层） |

### 与熔断器的关系
- **熔断器**（`SessionContext::CircuitState`）：流量层，瞬态。Degraded 设备熔断可仍 Closed，Open 熔断下设备仍可 Connected。HalfOpen 探测闭合即恢复 Closed。
- **生命周期**（`DeviceLifecycleState`）：设备层，持久。`SetLifecycle` 触发 `gauge` + `transitions counter` + INFO 日志（`[Lifecycle]` tag）。
- **互不替代**：`CanProceed()` 决定是否发起业务请求；`GetLifecycle()` 决定面板/告警如何呈现。

### 指标
```
# HELP myprot_device_lifecycle_state 设备生命周期 (0=New 1=Connecting 2=Connected 3=Degraded 4=Disabled)
# TYPE myprot_device_lifecycle_state gauge
myprot_device_lifecycle_state{device="PLC-001"} 2

# HELP myprot_device_lifecycle_transitions_total 设备生命周期转换次数
# TYPE myprot_device_lifecycle_transitions_total counter
myprot_device_lifecycle_transitions_total{device="PLC-001",from="New",to="Connecting"} 1
myprot_device_lifecycle_transitions_total{device="PLC-001",from="Connecting",to="Connected"} 1
```

### 仍待定项

- **`Disabled` 触发条件**：当前仅协议/握手 `BuildError` 即时触发；"重复 N 次 `Degraded` 自动 → `Disabled`" 的累积策略未启用，N 的取值与计数窗口未定。
- **`DeviceConfig.enabled` 字段**：尚未添加（当前 Config 范围封闭）。

> 登记在 [ADR-0003 未决问题登记](../adr/0003-known-issues.md)。

---

> §4.6 设备生命周期状态机与断路器（§4.5）正交，触发者表与故障分类一致。
> **上一节**: [Transport 模块](./03_Transport.md)
> **下一节**: [Gateway 模块](./05_Gateway.md)

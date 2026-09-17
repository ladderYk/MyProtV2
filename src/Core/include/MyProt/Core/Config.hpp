// src/Core/include/MyProt/Core/Config.hpp
// Config POCO — tagged struct 强类型配置 (C++11, ADR-0010 §3)
// 取代 std::variant; 判别联合体以 type 枚举 + 平铺字段表达

#pragma once
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdint>
#include "MyProt/Core/Optional.hpp"    // 自研 Core::Optional<T> (C++11 兼容, 取代 std::optional, 2026-08-29 回退 v140 不支持 <optional>)

#include "MyProt/Core/ByteOrder.hpp"   // LengthFieldConfig 依赖 ByteOrder 枚举
#include "MyProt/Core/SimulationConfig.hpp" // ServerConfig / 引用方 (ConfigDeepValidator) 仍需此头

namespace MyProt { namespace Core {

// ────────── 帧结构配置 (discriminated union) ──────────

struct LengthFieldConfig {
    int lengthFieldOffset;          // 长度字段在帧内的字节偏移 (≥0, 典型 2-4)
    int lengthFieldLength;          // 长度字段字节数 (1/2/4)
    bool lengthIncludesHeader;      // true: 长度值含 header 字节; false: 仅 body
    ByteOrder byteOrder;            // 长度字段字节序 (Modbus=BE, S7=LE, ...)
    int headerLength;               // 固定 header 字节数 (用于 body 起点定位)
    int lengthAdjustment;           // bodyLen = parsedLen + lengthAdjustment
                                    // (用于协议帧长≠实际数据长度时, e.g. 含 CRC 尾字节)
    int maxFrameSize;               // 运行时帧长度安全上限 (默认 1024, 防恶意超大帧)

    LengthFieldConfig()
        : lengthFieldOffset(0), lengthFieldLength(0), lengthIncludesHeader(false)
        , byteOrder(ByteOrder::BigEndian), headerLength(0), lengthAdjustment(0)
        , maxFrameSize(1024) {}
};

struct FixedConfig {
    int fixedLength;
    FixedConfig() : fixedLength(0) {}
};

struct SilenceConfig {               // 串口/RTU 静默成帧 (v1; 成帧由通道读循环驱动)
    int charTimeUs;                  // 单字符时间(微秒); 0 = 按协议级波特率/数据位自动折算 (仅诊断参考)
        int frameGapUs;                  // 帧间静默阈值(微秒); 须 > 0 显式配置 — 引擎不内置协议约定, 不自动折算 charTimeUs
    int maxFrameSize;

    SilenceConfig() : charTimeUs(0), frameGapUs(0), maxFrameSize(256) {}
};

struct MessageConfig {               // CAN 消息成帧 (预留; v1 校验器报名称错误)
    int maxFrameSize;                // 经典 CAN 8 / CAN FD 64
    int idFieldLength;               // 伪字节流头部 CAN ID 占用字节数

    MessageConfig() : maxFrameSize(8), idFieldLength(2) {}
};

enum class FramingType { LengthField, Fixed, Silence, Message };  // 对应 JSON 判别字段 "type"

/// 帧结构配置 — tagged struct (取代 std::variant, ADR-0010 §3);
/// type 决定生效分支, 不得读取未生效分支字段。
struct FramingConfig {
    FramingType type;
    LengthFieldConfig lengthField;   // type == LengthField 时有效
    FixedConfig fixed;               // type == Fixed 时有效
    SilenceConfig silence;           // type == Silence 时有效
    MessageConfig message;           // type == Message 时有效 (预留)

    FramingConfig() : type(FramingType::LengthField) {}
};

// ────────── 传输配置 ──────────

struct TcpTransportConfig {
    uint16_t defaultPort;         // 与 adl_serializer 缺省值一致 (Modbus TCP)
    TcpTransportConfig() : defaultPort(502) {}
};

struct TlsTransportConfig {
    uint16_t defaultPort;
    std::string certFile;
    std::string keyFile;
    std::string caFile;
    bool verifyServer;

    TlsTransportConfig() : defaultPort(0), verifyServer(true) {}
};

struct SerialTransportConfig {
    std::string portName;    // COM1, /dev/ttyUSB0
    uint32_t baudRate;
    uint8_t dataBits;
    enum class Parity { None, Odd, Even };
    Parity parity;
    enum class StopBits { One, Two };
    StopBits stopBits;

    SerialTransportConfig() : baudRate(9600), dataBits(8), parity(Parity::None), stopBits(StopBits::One) {}
};

enum class TransportType { Tcp, Tls, Serial };  // 对应 JSON 判别字段 "type"; CAN 预留不建模 (v1 校验器报名称错误)

/// 传输配置 — tagged struct (取代 std::variant, ADR-0010 §3);
/// type 决定生效分支, 不得读取未生效分支字段。
struct TransportConfig {
    TransportType type;
    TcpTransportConfig tcp;          // type == Tcp 时有效
    TlsTransportConfig tls;          // type == Tls 时有效
    SerialTransportConfig serial;    // type == Serial 时有效

    TransportConfig() : type(TransportType::Tcp) {}
};

// ────────── 响应解析器配置 ──────────

/// 解析器只输出原始字节; 最终类型转换由 tag.finalType 决定 (见 Config_Schema §6)
struct ResponseParserConfig {
    std::string validCondition;        // e.g. "resp[1]==0x03"; 空 = 跳过校验
    int dataStartIndex;
    std::string dataLengthExpr;        // e.g. "resp[2]"; 空 = 帧长 - dataStartIndex

    ResponseParserConfig() : dataStartIndex(0) {}
};

// ────────── 操作配置 ──────────

struct VariableConfig;   // 前置声明 — OperationConfig::variables 引用 (完整定义见下节)

struct OperationConfig {
    std::string name;                  // 由加载器从 operations map 的 key 回填, JSON 中不写
    std::vector<std::string> requestTemplate;
    ResponseParserConfig responseParser;
        // 操作语义标注 (可选, 空 = 未标注) — "read" | "write".
    //   运行时不依赖 (写路径按引用区分), 用于 UI 表单过滤与配置校验
    //   (标签 operation 应为 read 类, writeOperation 应为 write 类).
    std::string kind;
        // 输入/输出参数分组 (inputs / outputs) — 配置中不存在单段 variables:
    //   inputs : 操作者提供 / 运行时生成的输入 ( source=static 值或纯 UI 提示
    //            + source=auto{autoIncrement,frameSlice,expr,crc} ). 同层合并入
    //            RequestBuilder 扁平变量池供模板 {Name:Xn} 查表渲染.
    //   outputs: 由输入算出的派生输出 ( source=auto strategy=derivedLength, expr 引用
    //            "inputs ∪ {payload,count}" ). 参数层预解析按 expr 求值注入变量池, 模板仅查表.
    //   - 同一操作域内 inputs 与 outputs 不得同名 (校验 Error, 防"一个名字串场").
    //   - 跨操作允许角色变化: 同名变量在不同操作可以是 input 或 output
    //     (如 RegisterCount: 读操作=输入 / 写操作=输出), 合法且各自声明可见.
        //   - op.variables / op.placeholderHints 均不在当前 Schema 中 (代际变迁见 ADR-0005).
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;

    // 注: 单次请求-应答超时统一由 device.requestTimeoutMs 配置 (2026-08-24 收敛),
    //     协议操作不再持有 timeoutMs — 避免协议/设备两处超时漂移。
};

// ────────── 握手步骤 ──────────

struct HandshakeStep {
    std::string name;
    std::vector<std::string> requestTemplate;
    Optional<FramingConfig> framingOverride;   // 该步独立帧格式 (JSON null/缺失 = 使用通道默认)
    std::string validCondition;                     // 成功条件表达式
    std::string sessionExtractExpr;                 // 会话变量提取表达式 e.g. "resp[5:9]"
    std::string sessionVariable;                    // 提取后存入的变量名 e.g. "SessionID"
    int timeoutMs;                                  // 0 = 继承 device.connection.timeoutMs

    HandshakeStep() : timeoutMs(0) {}
};

// ────────── 协议变量声明 (inputs / outputs 两组) ──────────

/// 协议变量统一声明 — 一个变量在一处声明其来源 + 值/策略 + 展示元信息.
/// 取代旧三段 (defaultVariables / autoComputeJson / metadata.placeholderHints).
//
//   source 语义 (仅两种, 其余取值直接报错):
//     "static" — 声明. 含 value → 运行时注入 ctx.variables (标签 variables 同名键覆盖);
//                无 value → 纯展示元信息 (不进 ctx.variables, 仅渲染 UI 提示).
//     "auto"   — 自动计算. 模板 {Name:Xn}/{Name:raw} 消费其"参数层已解析值"
//                (计算下沉, 模板内无 :auto:/:calc: 令牌)
//
//   strategy (source=auto 时):
//     autoIncrement / frameSlice / expr / crc — 调用即自动求值 (拼进 autoComputeJson 喂 AutoComputeProvider)
//     derivedLength — 派生长度: 参数层按载荷字节数(payload) 的纯函数预解析,
//       只用 expr (e.g. "{WriteValue:len} + 7" / "{WriteValue:len} * 8"); 取代旧硬编码名族 (PayloadPlus4/
//       PayloadBits/FrameWithUnit 等已随 kind 字段一并移除).
//
//   source 必须显式声明. auto 需 strategy; static 需 value (uint32_t 或字符串 → 运行时按 size).
//
//   变长写载荷无需特殊策略声明 — 引擎扫描模板 {Name:raw} 占位符自动识别载荷,
//   把实际载荷字节数注入 {name:len} 解析表. inputs 中载荷名仅作 UI 展示 (source=static 无 value).
//
// JSON 形态(方案A: 参数分层，计算下沉):
//   "inputs": {
//     "UnitID":        { "source": "static", "value": 1, "label": "从站地址", "enum": [1..10] },
//     "TransactionID": { "source": "auto",   "strategy": "autoIncrement", "params": { "seed": 1 } }
//   },
//   "operations": {
//     "WriteMultipleRegisters": {
//       "inputs":  { "StartAddress": { "source": "static", "label": "起始寄存器地址" },
//                    "WriteValue":   { "source": "static", "label": "写入载荷" } },  // 仅 UI 展示; 载荷由模板 {WriteValue:raw} 识别
//       "outputs": { "PDULength": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len} + 7", "label": "PDU 长度" },
//                    "ByteCount": { "source": "auto", "strategy": "derivedLength", "expr": "{WriteValue:len}", "label": "数据字节数" } }
//     }
//   }
struct VariableConfig {
    std::string source;        // "static" | "auto"
    // static 段: 有值 → 运行时注入 ctx.variables; 无值 → 纯 UI 展示
    Optional<uint32_t> value;  // source=static 时可选 (有值才注入)
    // auto 段
    std::string strategy;      // source=auto 时必填: autoIncrement|frameSlice|expr|crc|derivedLength
    std::string paramsJson;    // source=auto 时有效 (derivedLength 不使用): 策略参数 (AutoComputeProvider 消费)
        // source=auto + strategy=derivedLength (唯一表达):
    //   expr: 基于载荷字节数的轻量算术表达式 (e.g. "{WriteValue:len} + 7", "{WriteValue:len} * 8");
    //   {name:len} 引用模板中变量的字节长度 (raw 载荷 = 实际字节数, 其余 = 模板渲染宽度);
    //   不依赖内置名词，全公式自解释。
    std::string expr;
    // 展示元信息 (所有 source 通用)
    std::string label;         // 显示名 (e.g. "从站地址")
    std::string unit;          // 单位 (e.g. "字节", "Hz", "个")
    /// 枚举候选成员: value = 实际参与渲染的值, label = 显示文本.
    ///   JSON 形态 (Config_Schema §3.2): 元素可为裸数字 (简写, label 取十进制字面量)
    ///   或 { "value": N, "label": "..." } 对象; 混合合法.
    struct EnumMember {
        uint32_t value;
        std::string label;
        EnumMember() : value(0) {}
    };
    std::vector<EnumMember> enumValues;   // 可选枚举候选 (UI 下拉; 空 = 不切换输入形态)
    std::string placeholder;   // 占位提示 (UI 输入框 placeholder)

    bool isStatic() const { return source == "static"; }
    bool isAuto()   const { return source == "auto"; }
    bool isDerivedLength() const { return isAuto() && strategy == "derivedLength"; }
};

/// 仿真配置 (协议级可选节) ──────────

/// 仿真操作行为描述见 SimulationConfig.hpp.

/// 协议级仿真配置; simulation 位于 server.json 顶层 ServerConfig.simulation.
/// 协议层彻底与 listenPort / initialValues / packetLossRate 等服务端行为脱耦.

/// ────────── 配置代际 ──────────

/// 受支持的配置代际号 (ADR-0005 版本门禁) — **单一真源**.
///   协议文件 / tags.json 根 / server.json 三者的 schemaVersion 必须精确等于此值
///   (缺省 = Warning + 假定为本值; 不符 = ConfigError Fail-Fast).
/// 该值的唯一来源在此; 装配点 (main.cpp / RuntimeGlue.cpp /
///   ConfigStoreOptions 默认值), 且 Core 注释引用了一个从未定义的常量
///   kSupportedSchemaVersion — 现补齐该常量并统一引用.
/// 变更此值须同步迁移 configs/ 下全部配置 (ADR-0005 迁移流程).
const int kSupportedSchemaVersion = 2;

/// ────────── 协议配置 ──────────

/// 标签按地址邻近合并的最大字节跨度缺省值 (TagGrouper::CoalesceAdjacent 消费).
///   这是"协议族单次读取上限"的通用表达: Modbus FC03 上限 125 寄存器 = 250 字节.
///   语义: 早期写死的 125 指"125 个寄存器"; 地址单位改为字节后, 同一物理跨度必须
///   字节 (StartByteAddress / ByteCount) 后该数值未同步调整, 实际只剩 125 字节
///   (≈62 寄存器), 否则批读合并能力静默缩水一半. 现为协议级 maxSpanBytes
///   并修正缺省值 (250 = 125 寄存器 × 2 字节).
const int kDefaultMaxSpanBytes = 250;

struct ProtocolConfig {
    std::string protocolName;
    TransportConfig transport;
    FramingConfig framing;
    Optional<ByteOrder> dataByteOrder; // 协议级数据解码字节序; 标签未显式声明 byteOrder 时回退至此; 未设置 = BigEndian
    std::unordered_map<std::string, OperationConfig> operations;
    std::vector<HandshakeStep> handshake;             // 填空数组 = 无握手 (Modbus)
        // 协议级 writeOperation / writeBytesOperation 不属于 Schema —
    //   写能力只在标签层声明 (标签级 writeOperation / writeBytesOperation /
    //   direction=write 三形态); 协议级字段曾使只读标签隐式可写 (legacy 兜底),
        //   与「配置即契约」相悖 (写声明点唯一: 标签).
        // simulation 段在 ServerConfig (server.json); 协议层不承载服务端行为.
        // 变量声明归一为 inputs / outputs 两组 (不存在 defaultVariables/autoComputeJson/placeholderHints 段).
        // inputs / outputs 按 source 分流:
    //     - inputs.source=static 有 value → 注入扁平变量池 (旧 static 语义); 无 value → 仅 UI (原 hint)
    //     - inputs.source=auto          → 拼装为 autoComputeJson 喂 AutoComputeProvider (非 derivedLength)
    //     - outputs.source=auto derivedLength → 参数层预解析按 expr 求值注入 (引用模板变量名, {name:len} 取字节长度)
        //     - 变长写载荷: 模板 {Name:raw} 占位符自动识别, 无需 inputs 特殊声明
    std::unordered_map<std::string, VariableConfig> inputs;
    std::unordered_map<std::string, VariableConfig> outputs;
    int schemaVersion;                        // 配置代际号 (ADR-0005); 须等于 kSupportedSchemaVersion
        // 变量别名映射 (alias → internal name).
    //   用户在协议 JSON 的 variableAliases 段自定义变量名, 引擎内部仍用契约名.
        //   可映射目标仅 2 个 — StartByteAddress / ByteCount (引擎真正查表读取的
    //     跨协议字节单位); 其余旧名已降级/移出契约, 见 Config_Schema.md §2.
    //   注意: 全局 variable-aliases.json 文件尚未实装 — 加载器只读协议文件内的本段.
    //   别名在加载期由 ConfigDirectoryLoader::ApplyVariableAliases 一次性归一化,
    //   运行期消费点无需感知; 本表仅作直接构造 ProtocolConfig (不经加载器) 路径的兜底.
    std::unordered_map<std::string, std::string> varAliasMap;
        // 标签按地址邻近合并的最大字节跨度 (语义见 kDefaultMaxSpanBytes).
    //   协议族"单次读取上限"在此统一表达, 引擎不再内建任何具体数字.
    //   注: v1 不区分读操作类型 (Modbus FC01 线圈上限 2000 远大于 FC03 寄存器上限 125),
    //       故协议作者应按其最紧的一类读操作取值.
    int maxSpanBytes;

    ProtocolConfig() : schemaVersion(kSupportedSchemaVersion)
                     , maxSpanBytes(kDefaultMaxSpanBytes) {}

    /// 把用户自定义名解析为引擎内部名; 无别名时返回原名.
    std::string ResolveVariableName(const std::string& name) const {
        auto it = varAliasMap.find(name);
        if (it != varAliasMap.end()) return it->second;
        return name;
    }
};

// ────────── 设备配置 ──────────

/// 连接参数 (扁平结构, 字段按协议 transport 类型取用; 适用性校验见 Config_Schema §7)
struct ConnectionConfig {
    std::string host;              // Tcp/Tls: 主机名或 IP
    uint16_t port;                 // Tcp/Tls: 0 = 使用协议 defaultPort
    std::string portName;          // Serial: 覆盖协议级 portName
    int timeoutMs;                 // 连接建立超时

    ConnectionConfig() : port(0), timeoutMs(3000) {}
};

/// 韧性策略 (Config_Schema §11 / ADR-0004)。全局块与设备级覆盖同构; 设备级省略字段回退全局值。
struct ResilienceConfig {
    int maxAttempts;           // 读操作总尝试次数(含首次); 写恒为 1
    int backoffBaseMs;         // 指数退避基数
    int backoffMaxMs;          // 单次退避上限
    int failureThreshold;      // 连续逻辑读取失败(预算耗尽) → 熔断打开
    int cooldownMs;            // 熔断打开持续时长, 期间 CircuitOpen 快速失败
    int halfOpenProbes;        // 半开态探测次数

    ResilienceConfig()
        : maxAttempts(3), backoffBaseMs(100), backoffMaxMs(1000)
        , failureThreshold(5), cooldownMs(10000), halfOpenProbes(1) {}
};

/// 管理面安全 (Config_Schema §12 / ADR-0008)。ConfigRoot 顶层可选块; nullopt = 全取默认。
struct WebApiConfig {
    std::string bindAddress;       // 监听地址; 默认仅环回, 远程管理需显式改 0.0.0.0
    std::string certFile;          // TLS 证书; 与 keyFile 齐备即启用 httplib::SSLServer
    std::string keyFile;           // TLS 私钥; 与 certFile 齐备即启用 TLS
    bool requireAuth;              // true 时 token 缺失即启动 Fail-Fast (生产建议 true)
    int rateLimitRps;              // 敏感端点令牌桶每秒速率
    int rateLimitBurst;            // 令牌桶突发上限; 超限 429
    std::string webRoot;           // 静态前端根目录 (Vue 构建产物); 空 = 不托管静态页面
    // 注: token 不入配置块, 取环境变量 MYPROT_API_TOKEN, 日志脱敏

    WebApiConfig()
        : bindAddress("127.0.0.1"), requireAuth(false)
        , rateLimitRps(5), rateLimitBurst(10), webRoot("webui/dist") {}
};

struct DeviceConfig {
    std::string id;
    std::string protocol;
    ConnectionConfig connection;
    int requestTimeoutMs;                  // 单次请求-应答超时(ms); 该设备所有操作共用 — 唯一配置点 (2026-08-24 收敛)
    Optional<std::string> username;  // 协议级认证凭据: 握手阶段作为模板变量注入, 日志脱敏
    Optional<std::string> password;
    Optional<ResilienceConfig> resilience; // 逐设备韧性覆盖; 未设置 = 用全局 ConfigRoot.resilience
    // 设备级模板变量缺省 (§4): 加载期合并入各标签有效变量集, 标签显式声明优先
    std::map<std::string, uint32_t> variables;

    DeviceConfig() : requestTimeoutMs(3000) {}
};

// ────────── 协议族约定键名 (引擎 ↔ 协议模板的接口约定) ──────────
// 引擎中所有协议名字面量收敛于以下约定函数 (跨协议字节单位 2 个,
//   另含 BitOffset 位偏移); 除此之外 Core 不含任何协议知识.
//   {StartByteAddress/ByteCount} 是跨协议统一的字节单位 (引擎内部);
//   {StartAddress/RegisterCount} 是协议族单位 (协议 JSON 模板按 derivedLength 从前两者推导;
//   Modbus: {StartByteAddress}/2 = 寄存器号; S7: 直接字节地址;
//   线圈类: StartByteAddress*8 + BitOffset = 线圈位地址).

/// 起始字节地址变量名 — 跨协议字节单位
inline std::string StartByteAddressVariableName() {
    return "StartByteAddress";
}

/// 数据区字节跨度变量名 — 跨协议字节单位
inline std::string ByteCountVariableName() {
    return "ByteCount";
}

// StartAddressVariableName / RegisterCountVariableName 不存在 —
//   协议族单位名 (StartAddress/RegisterCount) 由协议 JSON outputs(derivedLength) 自行声明,
//   引擎从不查表读取; 校验器的模板引用域白名单已改为从 protocol.outputs / op.outputs
//   动态收集派生名 (ConfigDeepValidator), 不再假定任何具体名.

/// 写路径默认注入的变量名 — 非契约名, 仅作 TagDefinition::writeVariable 的缺省值
///   (标签可用 writeVariable 覆盖; 模板按需用 {WriteValue:X4} 或 {WriteValue:raw} 消费).
const char* const kDefaultWriteValueVariable = "WriteValue";

/// 位偏移在 derivedLength expr 中的作用域名 — 非契约名.
///   协议 inputs 需声明同名 static 缺省 0 (满足 expr 引用域校验);
///   标签侧的值来自 TagDefinition::bitOffset, 由 TagReader 注入本键以覆盖协议缺省.
const char* const kBitOffsetExprVariable = "BitOffset";

// ────────── 帧结构保留名 (单一真源) ──────────
//   这些名字由引擎内建解释 — 协议变量名 / 标签变量名 / variableAliases 别名均不得
//   声明占用 (校验器报错, 见 ConfigDeepValidator 的保留名冲突检查).
//   唯一来源在此; 禁止在 ConfigDeepValidator / AutoComputeProvider /
//   FrameConsistencyCheck 的字面量集合; 现收敛于此, 并逐名常量化供各使用点直接引用.

const char* const kFramePrimitiveName = "Frame";      // {Frame:fixed} 模板原语 (模板固定段总宽)
const char* const kExprMagicFrameLen  = "__frameLen"; // expr 魔法变量 = 已生成字节数 (不含本段自身)
const char* const kExprMagicFrameEnd  = "__frameEnd"; // 同 __frameLen, 语义更清晰
const char* const kPropLength         = "len";        // {Name:len} 取变量字节宽度
const char* const kPropOffset         = "offset";     // {Name:offset} 取占位符首现偏移
const char* const kPropFixed          = "fixed";      // {Frame:fixed} 属性

/// 全部保留名 — 校验器的变量名 / 别名冲突检查共用.
const char* const kFrameReservedNames[] = {
    kFramePrimitiveName, kExprMagicFrameLen, kExprMagicFrameEnd,
    kPropLength, kPropOffset, kPropFixed
};
const int kFrameReservedNameCount =
    static_cast<int>(sizeof(kFrameReservedNames) / sizeof(kFrameReservedNames[0]));

/// 是否为帧结构保留名.
inline bool IsFrameReservedName(const std::string& name) {
    for (int i = 0; i < kFrameReservedNameCount; ++i) {
        if (name == kFrameReservedNames[i]) return true;
    }
    return false;
}

// ────────── 标签定义 ──────────

/// 读后换算器 (C1, ROADMAP #8 提级项): 采集值 → 工程值, 按数组序依次应用.
/// 仅读路径 (ResponseParser); 写路径不做逆向换算 (写语义歧义, 见 ROADMAP 备注).
/// v1 仅 scale: value' = value * k + b. 典型: 温度 0.1°C/bit → k=0.1, b=0;
///   4-20mA 标定 → k=(量程上限-下限)/27648, b=下限.
struct ScaleConverter {
    double k;   // 系数
    double b;   // 偏移

    ScaleConverter() : k(1.0), b(0.0) {}
};

struct TagDefinition {
    std::string name;                   // 全局唯一 (e.g. "PLC-001.Temperature")
    std::string deviceId;
    std::string operation;              // 操作名 (e.g. "ReadHoldingRegisters")
        // 标签变量表 — 跨协议字节单位:
    //   StartByteAddress / ByteCount 为引擎契约名 (TagGrouper 取址 / 派生长度引用);
    //   协议族单位 (Modbus 的 StartAddress/RegisterCount、S7 的 DB/偏移族) 不写在标签里 —
    //   由协议 JSON 的 outputs(derivedLength) 派生. 标签显式声明同名键会覆盖派生值 (校验器告警).
    std::unordered_map<std::string, uint32_t> variables;
    int scanRateMs;
        // registerCount 字段不存在 — 字节跨度由协议 outputs.ByteCount (derivedLength) 派生.
    //   引擎零硬编码 (无"寄存器 = 2 字节"假设); 跨协议字节跨度统一在协议 JSON 内表达.
    //   Modbus: variables.RegisterCount (协议族单位) → derivedLength.ByteCount = {RegisterCount} * 2
    //   S7:     variables.ByteCount (字节单位) 直接
    std::string finalType;              // 转换目标类型 (Config_Schema §6)
    Optional<ByteOrder> byteOrder; // 未设置 = 回退链: 协议 dataByteOrder → BigEndian
        // deadband / reportMode 不存在 — 上报过滤无消费落点 (唯一结果出口 onResults
    //   直连 LatestValueStore, 在此过滤会让"最新值缓存"失真); 见 ROADMAP 上报过滤项.
    bool coalesce;                      // 是否参与地址邻近合并; 单地址读(如 S7 ReadVar)设 false
        // 位偏移 (0-7), 语义 = StartByteAddress 所指字节内的位偏移; -1 = 未声明.
        //   一等字段 (不再用 variables["BitOffset"] 魔法键表达):
    //     - ResponseParser 的 Bool 位提取直接读本字段 (不再查变量表);
    //     - TagReader 在 bitOffset >= 0 时注入 expr 作用域键 kBitOffsetExprVariable,
    //       供协议 outputs derivedLength 引用 (如 Modbus FC05/FC15 位寻址派生).
    int bitOffset;
        // ── 写标签: direction="write" 时本标签定义一次写入而非采集点 ──
    // operation 直接指向写操作模板 (如 "WriteVar"/"WriteSingleRegister"),
    // variables 携带完整写语义 (TransportSize/Length/DBNumber/AddrLo/...),
    // 不参与轮询; /api/data/write 按 tag 名定向到本标签.
    std::string direction;              // "read" (默认, 参与轮询) | "write" (只写标签, 不参与轮询)
    std::string writeVariable;          // 数值/字节注入的模板变量名 (默认 "WriteValue",
                                        //   模板按需用 {WriteValue:X4} 或 {WriteValue:raw} 消费)
    std::string readBackTag;            // 写标签: 写后读回校验引用的读标签名
                                        //   (空 = 用写标签自身读语义原样重读;
                                        //   v1.x 已取代 "ReadHoldingRegisters" 硬编码约定)
                                        // 注: 请求-应答超时不在此配置, 统一由 device.requestTimeoutMs 决定 (2026-08-24 收敛)
        // ── 标签级写能力: direction=read 标签在此声明写语义 ──
    // 三形态: 只读 (两字段均空, 写 API 明确拒绝)
    //         读写 (writeOperation 非空 = 可标量写; writeBytesOperation 非空 = 可变长写)
    //         只写 (direction=write, operation 即写操作, 不参与轮询).
    // 写请求变量表 = defaultVariables → variables → writeVariables → 注入 {writeVariable};
    // 读回校验用自身 operation (readBackTag 无需配置).
        // 协议级 writeOperation/writeBytesOperation 兜底不存在 — 写声明点唯一: 标签.
    std::string writeOperation;         // 标量写 (POST value) 用的操作模板名 (如 "WriteVar")
    std::string writeBytesOperation;    // 变长写 (POST bytes) 用的操作模板名 (模板以 {Name:raw} 消费)
    std::unordered_map<std::string, uint32_t> writeVariables; // 写请求专用变量覆盖 (如 S7 写的 TransportSize/Length 与读不同)

    // 读后换算链 (可选; 空 = 不换算, 行为与旧版完全一致).
    //   仅对数值型 (整型/浮点) 结果生效; Bool/ByteArray/String 标签声明 converters 为校验错误.
    //   换算后值统一为 Double (TypedValue::d) — 上报/快照/写回比对的消费方均可读.
    std::vector<ScaleConverter> converters;

    TagDefinition()
        : scanRateMs(1000), finalType("UInt16")
        , coalesce(true)
        , direction("read"), writeVariable(kDefaultWriteValueVariable)
        , bitOffset(-1) {
        // converters 无内置实例 — 默认空链 = 不换算
    }
};

// ────────── 配置根 (对应 tags.json) ──────────

struct ConfigRoot {
    int schemaVersion;                          // 配置代际号 (ADR-0005); 须等于 kSupportedSchemaVersion
    Optional<ResilienceConfig> resilience; // 全局韧性策略 (Config_Schema §11 / ADR-0004); 未设置 = 全取默认
    Optional<WebApiConfig> webApi;         // 管理面安全 (Config_Schema §12 / ADR-0008); 未设置 = 全取默认
    std::vector<DeviceConfig> devices;
    std::vector<TagDefinition> tags;

    ConfigRoot() : schemaVersion(kSupportedSchemaVersion) {}
};

}} // namespace MyProt::Core

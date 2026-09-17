// src/Service/src/SchemaRegistry.cpp
// FieldDescriptor 注册表实现 (M2) — 四张静态表 + nlohmann JSON 序列化。
// 表内容与 docs/Config_Schema.md §6/§11/§12 契约保持一致。

#include "MyProt/Service/SchemaRegistry.hpp"

#include "MyProt/Core/Config.hpp"   // 契约名/默认值常量单一真源 (避免 schema 与引擎漂移)

#include <nlohmann/json.hpp>

namespace MyProt { namespace Service {

namespace {

using Fields = std::vector<FieldDescriptor>;

FieldDescriptor Plain(const std::string& name, const std::string& type, bool required,
                      const std::string& def, const std::string& desc) {
    FieldDescriptor fd;
    fd.name = name; fd.type = type; fd.required = required;
    fd.defaultValue = def; fd.description = desc;
    return fd;
}

FieldDescriptor EnumF(const std::string& name, bool required,
                      const std::string& def, const std::string& desc,
                      const std::vector<std::string>& values) {
    FieldDescriptor fd = Plain(name, "enum", required, def, desc);
    fd.enumValues = values;
    return fd;
}

FieldDescriptor ObjF(const std::string& name, bool required,
                     const std::string& desc, const Fields& children) {
    FieldDescriptor fd = Plain(name, "object", required, "", desc);
    fd.children = children;
    return fd;
}

FieldDescriptor ArrF(const std::string& name, bool required,
                     const std::string& desc, const Fields& elementShape) {
    FieldDescriptor fd = Plain(name, "array<object>", required, "", desc);
    fd.children = elementShape;
    return fd;
}

// ── 韧性块 (Config_Schema §11 / ADR-0004): 根级与设备级覆盖共用 ──
Fields ResilienceFields() {
    return Fields{
        Plain("maxAttempts", "int", true, "3", "读操作总尝试次数(含首次), >=1; 写恒为 1"),
        Plain("backoffBaseMs", "int", true, "100", "指数退避基数(ms), >=0"),
        Plain("backoffMaxMs", "int", true, "1000", "单次退避上限(ms), >= backoffBaseMs"),
        Plain("failureThreshold", "int", true, "5", "连续逻辑读取失败阈值, >=1; 达到即熔断打开"),
        Plain("cooldownMs", "int", true, "10000", "熔断打开持续时长(ms), >=0"),
        Plain("halfOpenProbes", "int", true, "1", "半开态探测次数, >=1")
    };
}

// ── 管理面安全块 (Config_Schema §12 / ADR-0008) ──
Fields WebApiFields() {
    return Fields{
        Plain("bindAddress", "string", false, "127.0.0.1",
              "监听地址, 须为合法 IP; 默认仅环回, 远程管理需显式改 0.0.0.0"),
        Plain("certFile", "string", false, "",
              "TLS 证书路径; 与 keyFile 同空或同非空 (齐备即启用 TLS)"),
        Plain("keyFile", "string", false, "",
              "TLS 私钥路径; 与 certFile 同空或同非空"),
        Plain("requireAuth", "bool", false, "false",
              "true 时 token 缺失即启动 Fail-Fast (生产建议 true)"),
        Plain("rateLimitRps", "int", false, "5",
              "敏感端点令牌桶每秒速率, >=1"),
        Plain("rateLimitBurst", "int", false, "10",
              "令牌桶突发上限, >= rateLimitRps; 超限返回 429"),
        Plain("webRoot", "string", false, "webui/dist",
              "静态前端根目录 (Vue 构建产物); 空 = 不托管静态页面")
    };
}

// ── 连接块 (设备层) ──
Fields ConnectionFields() {
    return Fields{
        Plain("host", "string", false, "", "Tcp/Tls: 主机名或 IP"),
        Plain("port", "uint16", false, "0", "Tcp/Tls: 0 = 使用协议 defaultPort"),
        Plain("portName", "string", false, "", "Serial: 覆盖协议级 portName"),
        Plain("timeoutMs", "int", false, "3000", "连接建立超时(ms)")
    };
}

} // anonymous namespace

// ────────── 根层字段 ──────────

const Fields& SchemaRegistry::RootFields() {
    static const Fields kFields = Fields{
        Plain("schemaVersion", "int", true, std::to_string(Core::kSupportedSchemaVersion),
              "配置代际号 (ADR-0005); 须等于当前受支持代际; 缺省按当前代际处理并给 Warning"),
        ObjF("resilience", false, "全局韧性策略; 未设置 = 全取默认", ResilienceFields()),
        ObjF("webApi", false, "管理面安全配置; 未设置 = 全取默认", WebApiFields()),
        Plain("devices", "array<object>", true, "", "设备列表, 见 device 节"),
        Plain("tags", "array<object>", true, "", "标签定义列表, 见 tag 节")
    };
    return kFields;
}

// ────────── 协议层字段 ──────────

const Fields& SchemaRegistry::ProtocolFields() {
    // transport / framing 均为扁平判别联合体: 判别字段 type 与分支字段同层,
    // 仅 type 对应分支的字段有效 (Config_Schema §2.1 / §2.2)
    static const Fields kTransportChildren = Fields{
        EnumF("type", true, "Tcp", "传输判别字段; 决定生效分支", {"Tcp", "Tls", "Serial"}),
        Plain("defaultPort", "uint16", false, "502",
              "Tcp: 设备未指定端口时的回退值; Tls: 默认 0 = 必须由设备显式指定"),
        Plain("certFile", "string", false, "", "Tls: 客户端证书路径, 空 = 不使用双向认证"),
        Plain("keyFile", "string", false, "", "Tls: 客户端私钥路径"),
        Plain("caFile", "string", false, "", "Tls: CA 证书路径"),
        Plain("verifyServer", "bool", false, "true", "Tls: 是否校验服务端证书"),
        Plain("portName", "string", true, "", "Serial: 协议级默认串口名, 可被设备级覆盖"),
        Plain("baudRate", "uint32", false, "9600", "Serial: 波特率, 必须 > 0"),
        Plain("dataBits", "uint8", false, "8", "Serial: 数据位, 取值 5~8"),
        EnumF("parity", false, "None", "Serial: 校验位", {"None", "Odd", "Even"}),
        EnumF("stopBits", false, "One", "Serial: 停止位", {"One", "Two"})
    };

    static const Fields kFramingChildren = Fields{
        EnumF("type", true, "LengthField",
              "成帧判别字段; Message 为 CAN 预留, v1 报名称错误",
              {"LengthField", "Fixed", "Silence", "Message"}),
        Plain("lengthFieldOffset", "int", false, "0", "LengthField: 长度域偏移(字节), >= 0"),
        Plain("lengthFieldLength", "int", true, "",
              "LengthField: 长度域宽度(字节), 取值 1/2/4"),
        Plain("lengthIncludesHeader", "bool", false, "false", "LengthField: 长度值是否含头部"),
        EnumF("byteOrder", false, "BigEndian", "LengthField: 长度域字节序",
              {"BigEndian", "LittleEndian", "WordBigByteLittle", "WordLittleByteBig"}),
        Plain("headerLength", "int", false, "0",
              "LengthField: 头部总长(字节); 0 或 >= lengthFieldOffset+lengthFieldLength"),
        Plain("lengthAdjustment", "int", false, "0",
              "LengthField: bodyLen = parsedLen + lengthAdjustment (帧长≠数据长度时使用)"),
        Plain("maxFrameSize", "int", false, "1024",
              "LengthField/Silence: 运行时帧长度安全上限(字节), 须 > 有效帧头长度"),
        Plain("fixedLength", "int", true, "", "Fixed: 定长帧长度(字节), > 0"),
        Plain("charTimeUs", "int", false, "0",
              "Silence: 单字符时间(us); 0 = 按波特率/数据位自动折算 (仅诊断参考)"),
        Plain("frameGapUs", "int", false, "0",
              "Silence: 帧间静默阈值(us), Silence 分支必须 > 0 — v1.27 起不再默认 3.5×charTimeUs (Modbus RTU 约定已从引擎移除, 请按协议显式填写)"),
        Plain("idFieldLength", "int", false, "2",
              "Message(CAN 预留): 伪字节流头部 CAN ID 占用字节数")
    };

    static const Fields kOperationChildren = Fields{
        Plain("requestTemplate", "array<string>", true, "",
              "请求模板行序列; 每行要么纯十六进制字面量, 要么单占位符"),
        EnumF("kind", false, "(未标注)",
              "操作语义标注: read=读操作 / write=写操作; "
              "供 UI 表单过滤与标签 operation/writeOperation 一致性校验",
              {"read", "write"}),
        ObjF("responseParser", false, "响应解析器", Fields{
            Plain("validCondition", "string", false, "",
                  "有效性条件表达式 e.g. resp[1]==0x03; 空 = 跳过校验"),
            Plain("dataStartIndex", "int", false, "0", "数据起始索引"),
            Plain("dataLengthExpr", "string", false, "",
                  "数据长度表达式 e.g. resp[2]; 空 = 帧长 - dataStartIndex")
        })
        // 注: 单次请求-应答超时统一由 device.requestTimeoutMs 配置 (2026-08-24 收敛)
    };

    static const Fields kHandshakeStepChildren = Fields{
        Plain("name", "string", true, "", "步骤名"),
        Plain("requestTemplate", "array<string>", true, "", "请求模板行序列"),
        ObjF("framingOverride", false,
             "该步独立帧格式 (JSON null/缺失 = 使用通道默认)", kFramingChildren),
        Plain("validCondition", "string", false, "", "成功条件表达式"),
        Plain("sessionExtractExpr", "string", false, "",
              "会话变量提取表达式 e.g. resp[5:9]"),
        Plain("sessionVariable", "string", false, "",
              "提取后存入的变量名 e.g. SessionID"),
        Plain("timeoutMs", "int", false, "0", "0 = 继承 device.connection.timeoutMs")
    };

    static Fields kFields = Fields{
        Plain("protocolName", "string", true, "", "协议名, 非空且全局唯一"),
        Plain("schemaVersion", "int", true, std::to_string(Core::kSupportedSchemaVersion),
              "配置代际号 (ADR-0005); 须等于当前受支持代际; 缺省按当前代际处理并给 Warning"),
        ObjF("transport", true, "传输配置", kTransportChildren),
        ObjF("framing", true, "成帧配置", kFramingChildren),
        EnumF("dataByteOrder", false, "BigEndian",
              "协议级数据解码字节序; 标签未显式声明 byteOrder 时回退至此 (与 framing.byteOrder 解耦)",
              {"BigEndian", "LittleEndian", "WordBigByteLittle", "WordLittleByteBig"}),
                // 协议级 writeOperation / writeBytesOperation 不属于 Schema —
        //   写能力只在标签层声明 (标签 writeOperation / writeBytesOperation / direction=write).
        Plain("maxSpanBytes", "int", false, "250",
              "标签按地址邻近合并的最大字节跨度 (TagGrouper::CoalesceAdjacent); "
              "即协议族单次读取上限, 如 Modbus FC03 = 125 寄存器 = 250 字节"),
                // 变量别名映射 (alias → internal name).
        //   用户可在协议 JSON 的 variableAliases 段自定义变量名, 引擎内部仍用约定名;
        //   加载期由 ConfigDirectoryLoader 一次性归一化为内部名 (协议 / 设备 / 标签三侧).
                //   可映射的契约名仅 2 个 — 引擎真正查表读取的跨协议字节单位.
        ObjF("variableAliases", false, "变量别名映射 (alias → internal name)",
        {
            Plain(Core::StartByteAddressVariableName(), "string", false, "",
                  "起始字节地址的别名; 引擎内部仍用 StartByteAddress"),
            Plain(Core::ByteCountVariableName(), "string", false, "",
                  "数据区字节跨度的别名; 引擎内部仍用 ByteCount")
        }),
        ObjF("operations", true,
             "操作名 -> 操作配置映射, 至少一项; 值结构见 children", kOperationChildren),
        ArrF("handshake", false,
             "握手步骤序列; 空数组 = 无握手 (Modbus); 元素结构见 children",
             kHandshakeStepChildren),
        ObjF("simulation", false,
              "协议级仿真节; listenPort=0 即未启用。报文格式知识仍来自 transport/framing/operations",
        {
            Plain("listenPort", "int", false, "0", "仿真监听端口 (仅环回); 0 = 不启用"),
            Plain("registerCount", "int", false, "65536", "数据区寄存器数量 1~65536"),
            Plain("initialValues", "map", false, "",
                  "寄存器初值 {地址:值} e.g. {\"0\":100}"),
            ObjF("operations", true, "操作名 → 仿真行为声明",
            {
                EnumF("kind", true, "read",
                      "数据方向: read=应答带数据区, write=请求写入数据区",
                      { "read", "write" }),
                Plain("addressVar", "string", true, "", "匹配变量名 → 数据区起始地址"),
                Plain("countVar", "string", false, "", "read: 匹配变量名 → 寄存器数量"),
                Plain("dataOffset", "int", false, "-1", "write: 请求内数据起始偏移"),
                Plain("responseTemplate", "array<string>", false, "",
                      "自定义应答模板; 非空时覆盖 echo 反向合成。"
                      "文法: hex 字面量 | {req:N:M} 请求回显 | {data} 数据区")
            })
        })
    };
    return kFields;
}

// ────────── 设备层字段 ──────────

const Fields& SchemaRegistry::DeviceFields() {
    static const Fields kFields = Fields{
        Plain("id", "string", true, "", "设备 ID, 全局唯一"),
        Plain("protocol", "string", true, "", "引用的协议名, 须存在于协议文件集"),
        ObjF("connection", true, "连接参数 (按协议 transport 类型取用)", ConnectionFields()),
        Plain("requestTimeoutMs", "int", false, "3000",
              "单次请求-应答超时(ms); 该设备所有操作共用 — 唯一配置点 (2026-08-24 收敛)"),
        Plain("username", "string", false, "", "握手阶段模板变量注入; 日志脱敏"),
        Plain("password", "string", false, "", "握手阶段模板变量注入; 日志脱敏"),
        ObjF("resilience", false,
             "逐设备韧性覆盖; 未设置 = 用全局 resilience", ResilienceFields()),
        Plain("variables", "map", false, "{}",
              "设备级模板变量缺省 (uint32); 标签 variables 未声明同名键时回退至此")
    };
    return kFields;
}

// ────────── 标签层字段 ──────────

const Fields& SchemaRegistry::TagFields() {
    static const Fields kFields = Fields{
        Plain("name", "string", true, "", "标签名, 全局唯一"),
        Plain("deviceId", "string", true, "", "所属设备 ID, 须存在于 devices"),
        Plain("operation", "string", true, "",
              "操作名, 须存在于所引协议的 operations"),
        Plain("variables", "map", false, "",
              "操作模板变量表 { 变量名: 无符号整数 } e.g. {\"StartAddress\":0}"),
                // ── 标签级写能力 (唯一写声明点) ──
        Plain("writeOperation", "string", false, "",
              "标量写 (POST value) 操作名 (须为所引协议的写类操作); 空 = 不可标量写, "
              "写 API 明确拒绝; 非空 = 读写标签, 读回校验用自身 operation"),
        Plain("writeBytesOperation", "string", false, "",
              "变长写 (POST bytes) 操作名 (须为所引协议的写类操作, 模板以 {Name:raw} "
              "消费载荷); 空 = 不可变长写"),
        Plain("writeVariables", "map", false, "",
              "写请求专用变量覆盖 { 变量名: 无符号整数 }, 在 variables 之上合并 "
              "e.g. S7 写的 TransportSize/Length 与读不同"),
                // ── 只写标签 ──
        EnumF("direction", false, "read",
              "read=参与轮询 (默认, 写能力由 writeOperation/writeBytesOperation 声明); "
              "write=只写标签 (operation 即写操作, 不参与轮询)",
              {"read", "write"}),
        Plain("writeVariable", "string", false, Core::kDefaultWriteValueVariable,
              "写值注入的模板变量名 (模板以 {Name:X?} 或 {Name:raw} 消费)"),
        Plain("readBackTag", "string", false, "",
              "仅写标签 (direction=write) 可配: 写后读回校验引用的读标签名"),
        Plain("scanRateMs", "int", false, "1000", "扫描周期(ms)"),
                // registerCount 顶层字段不存在; 走 variables.ByteCount (跨协议字节单位)
        //   Modbus tag: variables.ByteCount = N×2 (寄存器数 × 2); 协议 JSON 通过 derivedLength 派生 RegisterCount 给 FC03 命令字用
        //   S7 tag:     variables.ByteCount = N (直接字节数)
        EnumF("finalType", false, "UInt16", "转换目标类型 (原始数据不足时 TypeConversionError)",
              {"ByteArray", "UInt16", "Int16", "UInt32", "Int32",
               "UInt64", "Int64", "Float", "Double", "Bool", "String"}),
        EnumF("byteOrder", false, "(回退: 协议 dataByteOrder)",
              "数据解码字节序; 未设置走回退链",
              {"BigEndian", "LittleEndian", "WordBigByteLittle", "WordLittleByteBig"}),
        Plain("coalesce", "bool", false, "true", "是否参与地址邻近合并; 单地址读(如 S7 ReadVar)设 false"),
        Plain("bitOffset", "int", false, "-1",
              "位偏移 0-7 (语义 = StartByteAddress 所指字节内的位偏移); -1 = 未声明. "
              "Bool 位提取按本字段取位; 协议位寻址 derivedLength expr 通过注入的 BitOffset 引用"),
        Plain("converters", "array", false, "[]",
              "读后换算链 (C1): 采集值→工程值, 按序应用. v1 仅 scale 项 {kind:'scale',k,b} "
              "(value'=value*k+b, 结果为 Double); 仅数值型 finalType 的读标签可配 (规则13)")
        // 注: 请求-应答超时不在此配置, 统一由 device.requestTimeoutMs 决定 (2026-08-24 收敛)
    };
    return kFields;
}

// ────────── 序列化 ──────────

namespace {

nlohmann::json DescriptorToJson(const FieldDescriptor& fd) {
    nlohmann::json j;
    j["name"] = fd.name;
    j["type"] = fd.type;
    j["required"] = fd.required;
    if (!fd.defaultValue.empty()) {
        j["default"] = fd.defaultValue;
    }
    if (!fd.description.empty()) {
        j["description"] = fd.description;
    }
    if (!fd.enumValues.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 0; i < fd.enumValues.size(); ++i) {
            arr.push_back(fd.enumValues[i]);
        }
        j["enum"] = arr;
    }
    if (!fd.children.empty()) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 0; i < fd.children.size(); ++i) {
            arr.push_back(DescriptorToJson(fd.children[i]));
        }
        j["children"] = arr;
    }
    return j;
}

nlohmann::json SectionToJson(const Fields& fields) {
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < fields.size(); ++i) {
        arr.push_back(DescriptorToJson(fields[i]));
    }
    return arr;
}

} // anonymous namespace

std::string SchemaRegistry::ToJson(int supportedSchemaVersion) {
    nlohmann::json root;
    root["schemaVersion"] = supportedSchemaVersion;
    root["sections"]["root"] = SectionToJson(RootFields());
    root["sections"]["protocol"] = SectionToJson(ProtocolFields());
    root["sections"]["device"] = SectionToJson(DeviceFields());
    root["sections"]["tag"] = SectionToJson(TagFields());
    return root.dump();
}

}} // namespace MyProt::Service

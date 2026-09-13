// src/Service/include/MyProt/Service/ConfigValidator.hpp
// 配置深度校验器 — Config_Schema §7 十五项规则 + 版本门禁 + 韧性/管理面校验
// 主 API 操作 POCO 结构体; ValidateJson* / validateAndParse* 为 JSON 文本入口。

#pragma once
#include <string>
#include <vector>
#include <set>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Service {

/// 校验结果: 错误 (阻止启动) 与警告 (不阻止, 日志输出)
struct ValidationResult {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    bool hasErrors() const { return !errors.empty(); }

    void addError(const std::string& msg)   { errors.push_back(msg); }
    void addWarning(const std::string& msg) { warnings.push_back(msg); }

    /// 合并另一份结果 (warnings 合并; errors 合并后若对方有错误则整体 hasErrors)
    void merge(const ValidationResult& other) {
        errors.insert(errors.end(), other.errors.begin(), other.errors.end());
        warnings.insert(warnings.end(), other.warnings.begin(), other.warnings.end());
    }
};

/// 校验 + 解析一体产物: 单次 JSON 解析同时得到深度校验结果与 POCO
/// (供 ConfigDirectoryLoader 生产加载链路, 消除 validate+parse 双重解析)
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
    explicit ConfigValidator(int supportedSchemaVersion = 2);

    // ── POCO 入口 ──

    /// 校验单个协议 (对应 protocols/*.json)
    ValidationResult validateProtocol(const Core::ProtocolConfig& proto) const;

    /// 校验配置根 (对应 tags.json); 若传入协议列表则做跨文件引用校验
    ValidationResult validateConfigRoot(
        const Core::ConfigRoot& root,
        const std::vector<Core::ProtocolConfig>& protocols = std::vector<Core::ProtocolConfig>()) const;

    // ── JSON 文本入口 (供 ConfigStore 直接调用) ──

    /// 校验协议 JSON 文本
    static Core::Expected<ValidationResult> validateProtocolJson(
        const std::string& json, int supportedSchemaVersion = 2);

    /// 校验标签根 JSON 文本 (可附带协议 JSON 列表做跨文件引用)
    static Core::Expected<ValidationResult> validateConfigRootJson(
        const std::string& json,
        const std::vector<std::string>& protocolJsons = std::vector<std::string>(),
        int supportedSchemaVersion = 2);

    // ── JSON → POCO 解析 (供 ConfigDirectoryLoader 生产加载链路) ──
    static Core::Expected<Core::ProtocolConfig> parseProtocolJson(const std::string& json);
    static Core::Expected<Core::ConfigRoot> parseConfigRootJson(const std::string& json);

    // ── 试算校验 (ADR-0012 §2, 实现 FrameConsistencyCheck.cpp) ──
    /// 对协议内含 {Name:raw} 载荷占位符的 LengthField 写操作以真实管线试渲染一帧:
    ///   1) 派生长度 expr 可解性 (求值失败 → 错误, 封堵运行时静默 0 值);
    ///   2) 渲染帧与 framing 长度槽位一致性 (模板固定段 ↔ outputs 常量 ↔ framing 三方核对);
    ///   3) {Frame:fixed} 保留名 / 未知格式占位符拦截。
    /// 输入为已解析 POCO。挂接点: 保存期 ConfigStore::Validate +
    /// 加载期 vN 校验入口 (validateConfigRootJson / validateAndParseConfigRootJson)。
    /// @return 错误列表 (空 = 通过)
    static std::vector<std::string> CheckFrameConsistency(
        const Core::ProtocolConfig& proto);

    // ── 文法工具 (供同库其他 TU 复用, 如 ConfigDirectoryLoader 的仿真模板行校验) ──
    /// 十六进制字面量行校验: 每字节恰两位 hex, 空格/制表分隔, 至少一个 token。
    static bool isValidHexLiteral(const std::string& s);

    // ── 校验 + 解析一体入口 (单次 JSON 解析; 生产加载链路用) ──

    /// 协议 JSON: 深度校验并解析为 POCO
    static Core::Expected<ProtocolValidationOutcome> validateAndParseProtocolJson(
        const std::string& json, int supportedSchemaVersion = 2);

    /// 配置根 JSON: 深度校验并解析为 POCO。
    /// protocols 为已各自通过校验的协议集 — 仅执行根级规则与跨文件引用校验,
    /// 不再重复逐协议解析/校验 (由调用方在协议加载阶段完成一次即可)。
    static Core::Expected<ConfigRootValidationOutcome> validateAndParseConfigRootJson(
        const std::string& json,
        const std::vector<Core::ProtocolConfig>& protocols,
        int supportedSchemaVersion = 2);

private:
    int _supportedVersion;

    // ── 版本门禁 (唯一会提前终止的检查) ──
    bool checkVersionGate(int protoVersion, ValidationResult& r) const;

    // ── 协议层 (§7 规则 1-8) ──
    void validateTransport(const Core::TransportConfig& t, ValidationResult& r) const;
    void validateFraming(const Core::FramingConfig& f,
                         const Core::TransportConfig& t, ValidationResult& r) const;
    void validateHandshake(const std::vector<Core::HandshakeStep>& hs,
                           const Core::TransportConfig& transport,
                           ValidationResult& r) const;

    // ── 模板文法 (§3) ──
    void validateTemplate(const std::string& opName, const std::string& line,
                          ValidationResult& r) const;
    /// v1.13: 长度偏移自检 — 校验位于成帧长度槽位的派生变量相对 payload 的常量偏移与模板布局一致
    void ValidateDerivedLengthOffsets(const Core::ProtocolConfig& proto,
                                      const Core::OperationConfig& op,
                                      const std::string& opName,
                                      ValidationResult& r) const;

    // ── 设备层 (§7 规则 9-11) ──
    void validateDevice(const Core::DeviceConfig& dev,
                        const std::unordered_map<std::string,
                            const Core::ProtocolConfig*>& protoMap,
                        ValidationResult& r) const;

    // ── 标签层 (§7 规则 12-15) ──
    void validateTag(const Core::TagDefinition& tag,
                     const std::unordered_map<std::string,
                         const Core::DeviceConfig*>& deviceMap,
                     const std::unordered_map<std::string,
                         const Core::ProtocolConfig*>& protoMap,
                     ValidationResult& r) const;

    // ── 韧性策略 (§11) ──
    void validateResilience(const Core::ResilienceConfig& res,
                            const std::string& ctx, ValidationResult& r) const;

    // ── 管理面 (§12) ──
    void validateWebApi(const Core::WebApiConfig& api, ValidationResult& r) const;

    // ── 工具方法 ──
    static bool isValidPlaceholder(const std::string& s);
    static bool isValidIPAddress(const std::string& s);
    static bool isNumericFinalType(const std::string& ft);
    static bool isAllowedFinalType(const std::string& ft);
};

}} // namespace MyProt::Service

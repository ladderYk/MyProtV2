// src/Engine/include/MyProt/Engine/AutoComputeProvider.hpp
// 通用自动计算求值器 (protocol JSON 顶层 inputs.source=auto 声明驱动)
//  - 声明: 协议 JSON inputs.source=auto 段声明 {strategy: autoIncrement|frameSlice|expr|crc}
//  - 调用: RequestBuilder.RenderTemplate 遇 auto 占位符调 Resolve(name, byteWidth, ctx)
//         Resolve 内部: 按声明路由到策略; 未声明走原子自增兜底 (防御性, 正常流程不达)
//
// 4 个内置策略:
//   autoIncrement : 原子 +1 自增 (可种子化)               — modbus TransactionID
//   frameSlice    : 整包/段长度/字节切片                     — TPKT_Length/MBAP_Length
//   expr          : 算术表达式 (数字/变量/+-*/%&|^~括号)   — const + 任意算术
//   crc           : 多项式校验 (crc16-modbus / crc32)       — modbus-RTU CRC
//
// BuildContext (按值传入, 零依赖):
//   variables          : 当前变量池 (含 tag/协议 default/autoCompute 注入)
//   frameSoFar         : 当前正在构建的字节流 (frameSlice 需读取)
//
// 头文件不引入 nlohmann/json.hpp (避免 Engine 模块对 nlohmann 的依赖);
// Declare 接受 std::string (autoCompute 段 JSON 字符串), 内部 cpp 解析.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

namespace MyProt { namespace Engine {

// 极简 JSON 值类型 — 仅在 cpp 内定义完整, 这里前置声明以让头文件能用 const JVal&
struct JVal;

/// 一次模板构建的上下文 (请求/响应框架字节流 + 变量池)
struct BuildContext {
    /// 当前变量池 (标量), { "StartAddress": 100, "RegisterCount": 4, ... }
    const std::unordered_map<std::string, uint32_t>* variables = nullptr;
    /// 当前模板展开已生成的字节流 (frameSlice 读取)
    /// 注意: 该流在每次 :auto 求值时是"已生成前缀" (TPKT_Length 算整包时 = 全部前缀)
    const std::vector<uint8_t>* frameSoFar = nullptr;
};

/// 模板布局 (ADR-0012 §1.2) — 由 requestTemplate 一次扫描得出,
/// 供 {Frame:fixed} / {Name:offset} 派生长度求值与保存期试算校验共用.
///   widths     : 占位符名 → 渲染宽度 ({N:raw} = kRawMarker; 未知格式记 0 + hasUnknown)
///   offsets    : 占位符名 → 首次出现前累计字节偏移 (raw 载荷占位也记录)
///   fixedTotal : 全部非 raw 元素宽度之和 (帧固定段, 含各 Xn 占位符自身宽度)
///   hasUnknown : 出现未知格式占位符 (校验器应报错, 布局按 0 计)
struct TemplateLayout {
    static const uint32_t kRawMarker = 0xFFFFFFFFu;
    std::unordered_map<std::string, uint32_t> widths;
    std::unordered_map<std::string, uint32_t> offsets;
    uint32_t fixedTotal;
    bool hasUnknown;
    TemplateLayout() : fixedTotal(0), hasUnknown(false) {}
};

/// 自动计算求值器 — 协议 inputs.source=auto 声明驱动
///
/// 线程安全: counters / rules / exprCache 三份可变状态各受独立 mutex 保护;
///   策略执行与表达式求值均在锁外 (rules 以 shared_ptr<const> 发布, 读者取快照).
///   内容相同的重复 DeclareJson 走快路径 (免 JSON 解析), 适配"每请求同步规则"的调用模型.
class AutoComputeProvider {
public:
    AutoComputeProvider();
    ~AutoComputeProvider();

    /// 复位某变量的自增计数 (测试/运维助手)
    void Reset(const std::string& name);

    /// 加载 auto 声明段 (字符串形式)
    /// 例: "{\"TransactionID\":{\"strategy\":\"autoIncrement\",\"params\":{\"seed\":1}}}"
    /// 空串/非法: 静默忽略 (无 auto 声明)
    /// 解析失败: 返回 false (协议 JSON 整体仍可用, 仅 auto 段失效)
    /// 内容与上次声明相同: 直接返回 true (跳过解析与规则重建)
    bool DeclareJson(const std::string& autoComputeJson);

    /// 模板里碰到 :auto 时调: 优先用声明的策略, 无声明退回 Next() 自增
    uint64_t Resolve(const std::string& name, int byteWidth, const BuildContext& ctx);

    /// 检查: 某变量是否被 autoCompute 段声明 (调试/验证用)
    bool IsDeclared(const std::string& name) const;

    /// 判定某声明变量是否为 autoIncrement (参数层预解析用).
    ///   参数分层: autoIncrement/derivedLength 属"参数层"(渲染前预解析进参数表);
    ///   frameSlice/expr(引用 __frameLen)/crc 属"帧感知"(渲染期按帧求值, 无法下沉).
    bool IsAutoIncrement(const std::string& name) const;

    /// strategy=derivedLength 派生长度求值 (参数层预解析阶段消费)
    ///   表达形态唯一: 用 expr (如 "{Payload:len} + 7" / "{Payload:len} * 8"), 不再有 kind 字段.
    ///   inputs 为已合并的扁平输入参数池 (可为空指针) — expr 可引用其中任意已就绪输入名
    ///         (validator 已保证引用域).
    ///   {name:len} 引用 inputs 变量的字节长度 — 载荷变量=实际字节数(由调用方注入 varLen),
    ///         其余=模板渲染宽度; 普通名引用查 inputs 值池. 无 payload/count 保留名.
    ///   模板结构原语 (ADR-0012 §1.1) — {Frame:fixed} (模板非 raw 元素宽度合计,
    ///         查 layout->fixedTotal) 与 {name:offset} (占位符首现前累计偏移, 查 layout->offsets);
    ///         layout 为 nullptr 时两原语不可解析 (求值失败), {name:len} 与普通名引用不受影响.
    ///   成功返回 true 并写 out; 表达式求值失败返回 false (errMsg 非空时填充原因).
    static bool ResolveDerivedLength(
        const std::string& expr,
        const std::unordered_map<std::string, uint32_t>* inputs,
        const std::unordered_map<std::string, uint32_t>* varLen,
        const TemplateLayout* layout,
        uint32_t& out,
        std::string* errMsg = nullptr);

private:
    // PIMPL: 内部实现持 (counters, rules, exprCache), 定义见 .cpp
    struct Impl;
    std::unique_ptr<struct Impl> _impl;

    /// 原子自增计数器取下一个值 (模 2^(8*byteWidth)) — 内部助手,
    ///   被 ExecAutoIncrement 与 Resolve 的兜底路径使用.
    uint64_t Next(const std::string& name, int byteWidth);

    // 内部: 4 个策略实现 (JVal 极简 JSON 值类型在 cpp 端定义, 在本命名空间内 MyProt::Engine::JVal)
    uint64_t ExecAutoIncrement(const std::string& name, int byteWidth, const JVal& params);
    uint64_t ExecFrameSlice   (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
    uint64_t ExecExpr         (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
    uint64_t ExecCrc          (const std::string& name, int byteWidth, const JVal& params, const BuildContext& ctx);
};

}} // namespace MyProt::Engine

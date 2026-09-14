// src/Gateway/include/MyProt/Gateway/TagReader.hpp
// 标签读取器 — Gateway 层唯一管线持有者 (modules/05_Gateway.md §5.3)
// 单读/批量读/写共用同一套 RequestBuilder → IChannel::SendReceive →
// ResponseParser 装配 (Engine 模块提供无状态原语)。

#pragma once
#include <asio.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Transport/IChannel.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"
#include "MyProt/Engine/ResponseParser.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"
#include "MyProt/Gateway/MergedRequest.hpp"

namespace MyProt { namespace Gateway {

/// 写后读回校验参数 — 由调用方 (App 装配层) 解析组装:
///   readOp/readVars  构建读回请求的操作模板与变量表 (含地址/长度语义)
///   expectedBytes    期望数据字节 (标量按 finalType 编码; 变长 = 写入字节)
///   tagLabel         诊断上下文 (Error.context)
/// 数据区起点取 readOp.responseParser.dataStartIndex (≤0 视为配置缺失)。
struct WriteBackCheck {
    Core::OperationConfig readOp;
    std::unordered_map<std::string, std::uint32_t> readVars;
    std::vector<std::uint8_t> expectedBytes;
    std::string tagLabel;
        // 读回 Build 同步 autoCompute 规则用 (空 = 不动).
    //   RuntimeGlue::BuildWriteBackCheck 快照协议 autoCompute 段 (经 MergeOpAutoComputeJson) 传入.
        //   该字段已合并操作 inputs 中 source=auto 的覆盖项 (BuildWriteBackCheck 在
    //   装入 readOp 之后调用 MergeOpAutoComputeJson 处理, 不再只是 protocol 快照).
    std::string autoComputeJson;
};

/// 协议级 + op 级 autoCompute 合并 (公开给 App 层 RuntimeGlue 用).
///   协议级由 RequestBuilder::CollectAutoComputeJson 重建 (运行时从 inputs 提取,
///   排除了 outputs 派生输出 — 那由 WriteBytes 阶段特判注入).
///   op 级覆盖从 op.inputs 中 source=auto 的条目累加 (字段级覆盖同名变量).
///   协议级同名键被 op 级覆盖 (后键覆盖前键).
std::string MergeOpAutoComputeJson(const Core::ProtocolConfig& protocol,
                                   const Core::OperationConfig& op);

/// 标签读取器 — 单读 / 批量读 / 单写
class TagReader {
public:
    using BatchHandler = std::function<void(std::vector<Core::TagValue>)>;
    using WriteHandler = std::function<void(Core::VoidExpected)>;

    TagReader();

    /// 批量读取合并后的标签组
    /// 构建一次请求 → 发送 → 按各 tag 拆分响应
    void ReadBatch(const MergedRequest& merged,
                   const std::vector<Core::TagDefinition>& tags,
                   const Core::ProtocolConfig& protocol,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   BatchHandler handler);

        /// 单寄存器写 (/write): 按协议写操作模板构建请求 → 发送 →
    /// 校验 echo (validCondition), 不解析数据区。
    /// 写操作名约定 "WriteSingleRegister"; 变量表 = tag.variables +
        /// StartAddress + {valueVariable}=value (变量名由调用方指定,
    /// 写标签用 tag.writeVariable, legacy 固定 "WriteValue")。
        /// backCheck 非空: 写 echo 校验通过后立即按
    /// backCheck->readOp/readVars 构建读请求, 以
    /// readOp.responseParser.dataStartIndex 为数据区起点,
    /// 逐字节比较 expectedBytes; 不一致回调 ReadBackMismatch。
    /// 空指针 = 不做读回。
    // 注: 原 P1 D busStrand 参数于 2026-08-29 撤回,见 ADR-0011 §3.2
    void WriteOnce(const Core::TagDefinition& tag,
                   Core::ProtocolConfig protocol,
                   const std::string& writeOperation,
                   std::uint32_t value,
                   const std::string& valueVariable,
                   const std::shared_ptr<const WriteBackCheck>& backCheck,
                   Transport::IChannel& channel,
                   int requestTimeoutMs,
                   WriteHandler handler);

    /// 变长字节写 (P1 A, ADR-0007 §3 销账项 /write bytes):
    /// 复用与 WriteOnce 同一管线; 额外接受 hex 字符串表注入到
    /// {Name:raw} 占位符 (如 Modbus FC16 多寄存器写、S7 ANY 指针)。
    /// 写操作名由调用方指定 (如 "WriteMultipleRegisters");
    /// 变量表 = tag.variables + StartAddress + variableBytesHex 内容。
        /// backCheck 语义同 WriteOnce (读回不复用写 op 模板 —
        /// 由调用方显式给读 op + 期望字节 — 复用写模板会发出错误请求)。
    void WriteBytes(const Core::TagDefinition& tag,
                    Core::ProtocolConfig protocol,
                    const std::string& writeOperation,
                    const std::unordered_map<std::string, std::string>& variableBytesHex,
                    const std::shared_ptr<const WriteBackCheck>& backCheck,
                    Transport::IChannel& channel,
                    int requestTimeoutMs,
                    WriteHandler handler);

private:
        /// 写后读回统一执行体: 构建 check->readOp 读请求 → 发送 →
    /// 以 readOp.responseParser.dataStartIndex 为数据区起点,
    /// 逐字节比较 check->expectedBytes。终态恰好回调 done 一次。
    void RunReadBack(Transport::IChannel& channel,
                     const std::shared_ptr<const Core::FramingConfig>& framing,
                     int requestTimeoutMs,
                     const std::shared_ptr<const WriteBackCheck>& check,
                     const std::unordered_map<std::string, std::string>& varAliasMap,
                     const WriteHandler& done);
    /// P1 C (ADR-0011): per-device 写互斥位.
    /// 同一 device 上写与写互斥; 读不参与. 入口抢位; 单一 callback
    /// 路径 (成功/失败/超时 终态) 释放. 不依赖 io 线程模型,未来
    /// 切多 io_context 线程 run 也生效.
    std::unordered_map<std::string, std::atomic<bool>> _writingInFlight;

    Engine::RequestBuilder _requestBuilder;
    Engine::ResponseParser _responseParser;
    Engine::AutoComputeProvider _autoProvider;
};

}} // namespace MyProt::Gateway

// src/Gateway/include/MyProt/Gateway/WriteService.hpp
// 写路径服务（P2-2）— 自 App/DataApi.cpp 下沉：
//   写请求的可写性判定、finalType 编解码、读回校验组装、异步写入与等待。
// App 层只保留 HTTP body 解析与错误码映射；Gateway 单测可直接覆盖写路径。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <asio.hpp>

#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/RuntimeSnapshot.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Gateway/TagReader.hpp"   // WriteBackCheck

namespace MyProt { namespace Gateway {

/// 单个写请求 — value 与 bytes 互斥（bytes 非空 → 变长写）
struct WriteRequest {
    std::string tagName;
    double value;
    std::vector<std::uint8_t> bytes;
    bool readBack;

    WriteRequest() : value(0.0), readBack(false) {}
};

/// 写路径服务：每次 Write 取当前运行时快照并保活到异步链结束（热重载整代替换安全）。
/// 非线程安全；由调用方在需要的位置构造（通常是 WebApi 工作线程，内部 post 到 io 线程执行）。
class WriteService {
public:
    WriteService(asio::io_context& io, ProtocolGateway& gateway,
                 Core::RuntimeSnapshotHolder& snapshot);

    /// 同步等待写完成（含可选读回校验）；错误码语义与原 App 实现一致。
    Core::VoidExpected Write(const WriteRequest& req);

    // ── 纯函数（可单测；HTTP 层不重复实现） ──
    static bool EncodeFinalTypeToBytes(const std::string& ft, double v,
                                       Core::ByteOrder bo,
                                       std::vector<std::uint8_t>& out,
                                       std::string& errMsg);
    static bool CheckIntegerWriteValue(const std::string& ft, double v,
                                       std::string& errMsg);
    /// 写操作模板是否必须走 BuildBytes 路径（{Name:raw} 或引用 {name:len} 的派生长度）
    static bool OpNeedsBytePath(const Core::ProtocolConfig& proto,
                                const Core::OperationConfig& op,
                                const std::string& valueVariable);
    /// 组装写后读回参数：readBackTag 优先，缺省用写标签自身的读语义
    static bool BuildWriteBackCheck(
        const std::vector<Core::TagDefinition>& tags,
        const Core::ProtocolConfig& proto,
        const Core::TagDefinition& writeTag,
        double value,
        const std::vector<std::uint8_t>& bytes,
        WriteBackCheck& out,
        std::string& errMsg);

private:
    asio::io_context& _io;
    ProtocolGateway& _gateway;
    Core::RuntimeSnapshotHolder& _snapshot;
};

}} // namespace MyProt::Gateway

// src/App/DataApi.cpp — /api/data/* 实时数据面 + 写路径 (方案1-S2 自 RuntimeGlue.cpp 逐字节搬移, 零行为变更)
// 内容: TypedToJson / BuildLatestJson / HandleDataApi + 写路径辅助 (OpNeedsBytePath / EncodeFinalTypeToBytes
//       / CheckIntegerWriteValue / BuildWriteBackCheck / WriteViaGateway) + HandleWriteApi。

#include <atomic>
#include <csignal>
#include <cstring>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <future>
#include <chrono>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <memory>
#include <fstream>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <crtdbg.h>
#include <asio.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/ServerConfig.hpp"  // LoadedConfig.server
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Gateway/TagReader.hpp"     // WriteBackCheck
#include "MyProt/Gateway/TagGrouper.hpp"    // GetStartAddress (读回变量表)
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Engine/ResponseParser.hpp"   // 写路径 byteOrder 裁决共用 (读写一致)
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "RuntimeGlue.hpp"
#include "AppSignals.hpp"
#include "CrashDiagnostics.hpp"
#include <nlohmann/json.hpp>

namespace MyProt { namespace App {
    // TypedValue → nlohmann::json (ByteArray 转十六进制字符串, Empty → null)
    nlohmann::json TypedToJson(const MyProt::Core::TypedValue& tv) {
        using MyProt::Core::ValueType;
        switch (tv.type) {
            case ValueType::UInt16:
            case ValueType::UInt32:
            case ValueType::UInt64:  return nlohmann::json(tv.u);
            case ValueType::Int16:
            case ValueType::Int32:
            case ValueType::Int64:   return nlohmann::json(tv.i);
            case ValueType::Float:
            case ValueType::Double:  return nlohmann::json(tv.d);
            case ValueType::Bool:    return nlohmann::json(tv.b);
            case ValueType::String:  return nlohmann::json(tv.str);
            case ValueType::ByteArray: {
                std::string hex;
                static const char* kDigits = "0123456789ABCDEF";
                for (size_t k = 0; k < tv.bytes.size(); ++k) {
                    hex.push_back(kDigits[(tv.bytes[k] >> 4) & 0xF]);
                    hex.push_back(kDigits[tv.bytes[k] & 0xF]);
                }
                return nlohmann::json(hex);
            }
            default: return nlohmann::json();
        }
    }

    // ── latest 快照 → JSON ({count, tags:[...]}) — /latest 与 SSE /stream 共用 ──
    std::string BuildLatestJson(
            const MyProt::Polling::LatestValueStore& latestStore,
            const std::string& deviceFilter) {
        const std::vector<MyProt::Core::TagValue> snap =
            latestStore.Snapshot(deviceFilter);

        nlohmann::json tagsArr = nlohmann::json::array();
        for (size_t i = 0; i < snap.size(); ++i) {
            const MyProt::Core::TagValue& tv = snap[i];
            nlohmann::json item;
            item["name"] = tv.tagName;
            item["device"] = tv.deviceId;
            item["value"] = TypedToJson(tv.typedValue);
            item["quality"] =
                tv.quality == MyProt::Core::QualityCode::Good ? "Good"
                : (tv.quality == MyProt::Core::QualityCode::Uncertain
                       ? "Uncertain" : "Bad");
            item["timestamp"] = tv.timestamp;
            item["changed"] = tv.valueChanged;
            tagsArr.push_back(item);
        }
        nlohmann::json out;
        out["count"] = snap.size();
        out["tags"] = tagsArr;
        return out.dump();
    }

    // ── /api/data/* — 实时数据快照面 (App 层注入的第二个扩展路由域) ──
    std::pair<int, std::string> HandleDataApi(
            const HttpRequest& req,
            const MyProt::Polling::LatestValueStore& latestStore) {
        const std::string& method = req.method;
        const std::string& path = req.path;
        typedef std::pair<int, std::string> Resp;

        if (path.rfind("/api/data/latest", 0) != 0)
            return Resp(404, "{\"error\":\"unknown data endpoint\"}");
        if (method != "GET")
            return Resp(405, "{\"error\":\"method not allowed\"}");

        return Resp(200, BuildLatestJson(latestStore, QueryParam(path, "device")));
    }

        // ── 写标签辅助 ──

    // finalType 是否浮点族 (Float/Double → IEEE754 大端字节编码)
    bool IsFloatFinalType(const std::string& ft) {
        return ft == "Float" || ft == "Double";
    }

        // 判断写操作模板是否以 {Name:raw} 消费写值 —
    // S7 WriteVar 等变长模板的整数标量也须走 WriteBytes (BuildBytes) 路径:
    // WriteOnce 标量路径不派生 PDULength/DataLen/DataLength/DataBits,
    // 模板构建即报 "模板变量未提供 (PDULength)"。
    bool OpConsumesRawValue(const MyProt::Core::OperationConfig& op,
                            const std::string& valueVariable) {
        const std::string part = "{"
            + (valueVariable.empty() ? Core::kDefaultWriteValueVariable : valueVariable) + ":raw}";
        for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
            if (op.requestTemplate[i] == part) return true;
        }
        return false;
    }

    // 从模板段 "{Name[:spec]}" 提取变量名 (冒号前); 非占位符段返回空串.
    // 模板段约定: 最外层 { ... }, 内部以 ':' 分段. "{Name:auto:X4}" → "Name".
    std::string ExtractPlaceholderName(const std::string& part) {
        if (part.size() < 2 || part.front() != '{' || part.back() != '}') {
            return std::string();
        }
        const std::string inner = part.substr(1, part.size() - 2);
        const size_t colon = inner.find(':');
        return (colon == std::string::npos) ? inner : inner.substr(0, colon);
    }

        // 写 op 是否需要走 BuildBytes 路径.
    // 条件: 模板以 {Name:raw} 消费写值, 或模板引用了「依赖载荷长度」的派生量
    //   (expr 含 {name:len} — 仅 BuildBytes 有 payload 上下文可求值, 如 PDULength 族;
        //   单条件判定: 覆盖 S7 模板 {PDULength:X4} 这类派生量占位 (只查 raw 会漏)
    //   不含 {WriteValue:raw} 时被误判走 Build 标量路径的问题).
        // 收紧: 不能把「模板引用任何派生量」都判为字节路径 — 仅由静态量
    //   派生的值 (如 Modbus {StartAddress} = StartByteAddress/2) 在 WriteOnce 的
    //   InjectDerivedLengthVariables 同样可解析; 误判使整数标量写被推入 BuildBytes,
    //   而该路径的 uint32 变量表不含 WriteValue (载荷以 hex 进 variableBytesHex),
    //   {WriteValue:X4} 渲染报 "模板变量未提供 [WriteValue]" — FC06 标量写全数失败
        //   (E2E 17-3 / 20a 长期为红的根因).
    bool OpNeedsBytePath(const MyProt::Core::ProtocolConfig& proto,
                         const MyProt::Core::OperationConfig& op,
                         const std::string& valueVariable) {
        if (OpConsumesRawValue(op, valueVariable)) return true;
        // 名字 → 派生量声明 (expr)
        std::unordered_map<std::string, const std::string*> derivedExpr;
        for (auto it = proto.outputs.begin(); it != proto.outputs.end(); ++it)
            if (it->second.isDerivedLength()) derivedExpr[it->first] = &it->second.expr;
        for (auto it = op.outputs.begin(); it != op.outputs.end(); ++it)
            if (it->second.isDerivedLength()) derivedExpr[it->first] = &it->second.expr;
        if (derivedExpr.empty()) return false;
        for (size_t i = 0; i < op.requestTemplate.size(); ++i) {
            const std::string name = ExtractPlaceholderName(op.requestTemplate[i]);
            if (name.empty()) continue;
            std::unordered_map<std::string, const std::string*>::const_iterator dit =
                derivedExpr.find(name);
            if (dit == derivedExpr.end()) continue;
            // 仅「expr 依赖载荷字节长度」的派生量才需要 payload 上下文.
            // 注: 派生 expr 不支持嵌套引用其他派生量, 无传递依赖需要展开.
            if (dit->second->find(":len}") != std::string::npos) return true;
        }
        return false;
    }

    // 字节序列 → 空格分隔大写 hex ("3F 80 00 00")
    std::string BytesToHexSpace(const std::vector<std::uint8_t>& b) {
        static const char kHex[] = "0123456789ABCDEF";
        std::string hex;
        hex.reserve(b.size() * 3);
        for (size_t i = 0; i < b.size(); ++i) {
            if (i) hex += ' ';
            hex += kHex[(b[i] >> 4) & 0xF];
            hex += kHex[b[i] & 0xF];
        }
        return hex;
    }

    // finalType → 按数据字节序编码线上字节 (写标签 Float/Double 数值路径 + 读回期望值).
    // 写/读共用同一 byteOrder 裁决链 (tag.byteOrder → protocol.dataByteOrder → 大端),
    // 保证写入字节与读路径解析字节一致 (修复 Modbus WordBigByteLittle 等混合序读写不一致).
    // 成功返回 true; 整数族含范围/整型校验, 越界或非法类型填充 errMsg.
    bool EncodeFinalTypeToBytes(const std::string& ft, double v,
                                Core::ByteOrder bo,
                                std::vector<std::uint8_t>& out,
                                std::string& errMsg) {
        out.clear();
        if (ft == "Float") {
            const float f = static_cast<float>(v);
            std::uint32_t u = 0;
            std::memcpy(&u, &f, sizeof(u));
            out = Core::ToBytes(u, bo);
            return true;
        }
        if (ft == "Double") {
            std::uint64_t u = 0;
            std::memcpy(&u, &v, sizeof(u));
            out = Core::ToBytes(u, bo);
            return true;
        }
        if (ft == "Bool") {
            out.push_back(v != 0.0 ? 1 : 0);
            return true;
        }
        if (ft == "UInt16" || ft == "Int16") {
            if (v != std::floor(v) || v < -32768.0 || v > 65535.0) {
                errMsg = "value 超出 " + ft + " 范围 [-32768,65535] 或非整数";
                return false;
            }
            const std::int64_t iv = static_cast<std::int64_t>(v);
            const std::uint16_t u = (ft == "Int16")
                ? static_cast<std::uint16_t>(static_cast<std::int16_t>(iv))
                : static_cast<std::uint16_t>(iv);
            out = Core::ToBytes(u, bo);
            return true;
        }
        if (ft == "UInt32" || ft == "Int32") {
            if (v != std::floor(v) || v < -2147483648.0 || v > 4294967295.0) {
                errMsg = "value 超出 " + ft + " 范围 [-2147483648,4294967295] 或非整数";
                return false;
            }
            const std::int64_t iv = static_cast<std::int64_t>(v);
            const std::uint32_t u = (ft == "Int32")
                ? static_cast<std::uint32_t>(static_cast<std::int32_t>(iv))
                : static_cast<std::uint32_t>(iv);
            out = Core::ToBytes(u, bo);
            return true;
        }
        if (ft == "UInt64" || ft == "Int64") {
            if (v != std::floor(v) || v < -9223372036854775808.0
                    || v >= 9223372036854775808.0) {
                errMsg = "value 超出 " + ft + " 范围或非整数";
                return false;
            }
            const std::uint64_t u = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(v));
            out = Core::ToBytes(u, bo);
            return true;
        }
        errMsg = "不支持的 finalType 编码: " + ft;
        return false;
    }

    // 写标签整数标量的范围校验 (走 {Name:X?} 格式化占位符, 宽度 ≤ 4 字节;
    // 64 位整数精度受限, 引导使用 bytes 路径). 成功返回 true.
    bool CheckIntegerWriteValue(const std::string& ft, double v,
                                std::string& errMsg) {
        if (v != std::floor(v)) {
            errMsg = "value 须为整数 (" + ft + ")";
            return false;
        }
        if (ft == "Bool") {
            if (v < 0.0 || v > 255.0) { errMsg = "value 须在 [0,255]"; return false; }
            return true;
        }
        if (ft == "UInt16" || ft == "Int16") {
            if (v < -32768.0 || v > 65535.0) {
                errMsg = "value 超出 " + ft + " 范围 [-32768,65535]";
                return false;
            }
            return true;
        }
        if (ft == "UInt32" || ft == "Int32") {
            if (v < -2147483648.0 || v > 4294967295.0) {
                errMsg = "value 超出 " + ft + " 范围 [-2147483648,4294967295]";
                return false;
            }
            return true;
        }
        if (ft == "UInt64" || ft == "Int64") {
            errMsg = "64 位整数标量写精度受限, 请改用 bytes 路径";
            return false;
        }
        errMsg = "写标签 finalType 不支持标量写: " + ft + " (Float/Double 走浮点编码)";
        return false;
    }

        // 组装写后读回校验参数.
    // 读标签选择: 写标签 readBackTag 优先; 未配置时用写标签自身 —
    //   legacy 场景下写目标即读标签, 其 operation 本就是读 op,
        //   以其自身读语义原样重读 (不复用 "ReadHoldingRegisters"
    //   约定查找与硬编码 FC03 布局解析).
    // 期望字节: bytes 非空 = 写入字节; 否则按读标签 finalType 编码 value.
    // 数据区起点由读 op 的 responseParser.dataStartIndex 提供 (RunReadBack).
    bool BuildWriteBackCheck(
            const std::vector<MyProt::Core::TagDefinition>& tags,
            const MyProt::Core::ProtocolConfig& proto,
            const MyProt::Core::TagDefinition& writeTag,
            double value,
            const std::vector<std::uint8_t>& bytes,
            MyProt::Gateway::WriteBackCheck& out,
            std::string& errMsg) {
        const MyProt::Core::TagDefinition* readTag = &writeTag;
        if (!writeTag.readBackTag.empty()) {
            readTag = 0;
            for (size_t i = 0; i < tags.size(); ++i) {
                if (tags[i].name == writeTag.readBackTag
                        && tags[i].direction != "write") {
                    readTag = &tags[i];
                    break;
                }
            }
            if (!readTag) {
                errMsg = "readBackTag 引用的读标签未找到: " + writeTag.readBackTag;
                return false;
            }
        }
        std::unordered_map<std::string, MyProt::Core::OperationConfig>::const_iterator
            opIt = proto.operations.find(readTag->operation);
        if (opIt == proto.operations.end()) {
            errMsg = "读回操作未找到: " + readTag->operation
                + " (protocol: " + proto.protocolName + ")";
            return false;
        }
        out.readOp = opIt->second;
        out.readVars = MyProt::Engine::RequestBuilder::MergeVariables(
            MyProt::Engine::RequestBuilder::CollectStaticVariables(proto),
            readTag->variables);
                // 注入跨协议字节单位; 协议族地址 (如 Modbus StartAddress 寄存器号)
        //   由读 op outputs 派生 — 与 ReadBatch/Write 路径同一 Inject 管线.
        out.readVars[Core::StartByteAddressVariableName()] =
            MyProt::Gateway::TagGrouper::GetStartAddress(*readTag);
        out.readVars[Core::ByteCountVariableName()] =
            MyProt::Gateway::TagGrouper::GetByteCount(*readTag);
        {
            const MyProt::Engine::TemplateLayout layout =
                MyProt::Engine::RequestBuilder::BuildTemplateLayout(
                    out.readOp.requestTemplate);
            MyProt::Engine::RequestBuilder::InjectDerivedLengthVariables(
                out.readVars, proto.outputs, out.readOp.outputs,
                /*totalBytes=*/0, layout);
        }
        out.tagLabel = writeTag.name;
                // 读回也是 op 级 (readOp), 需合并 op.inputs 中 source=auto
        //   的覆盖项 — 与 TagReader::Read/Write 调用点走同一 SyncAutoComputeRules 路径.
        out.autoComputeJson = MyProt::Gateway::MergeOpAutoComputeJson(proto, out.readOp);
        if (!bytes.empty()) {
            out.expectedBytes = bytes;
            return true;
        }
        return EncodeFinalTypeToBytes(readTag->finalType, value,
                MyProt::Engine::ResponseParser::ResolveByteOrder(proto, readTag->byteOrder),
                out.expectedBytes, errMsg);
    }

        // ── /api/data/write 写路径同步封装 (含写标签扩展) ──
    // WebApi 线程 → io.post 全链 (查标签/设备/协议 → GOC → WriteOnce/WriteBytes),
    // 与轮询回调在 io 线程串行 — 消除跨线程 socket 操作与配置向量竞争。
    // promise 用 shared_ptr 保活: 超时返回后 lambda 可能仍在 io 队列中执行。
    // bytes 路径 (P1 A 销账项): 非空时走 WriteBytes 变长写, 透传 {Name:raw} 注入表。
        // 写标签 (tag.direction == "write"):
    //   - 写 op = tag.operation (不再依赖协议级 writeOperation 约定,
    //     修复 S7 等无 writeOperation 协议标量写必失败的问题)
    //   - 数值注入变量 = tag.writeVariable (默认 "WriteValue")
    //   - Float/Double finalType → IEEE754 大端字节 → raw 注入 (WriteBytes 路径)
    //   - readBack: 按 readBackTag (缺省写标签自身) 的读 op 重读,
    //     以读 op dataStartIndex 为起点逐字节比较 (RunReadBack)
    MyProt::Core::VoidExpected WriteViaGateway(
            const AppContext& ctx,
            const std::string& tagName,
            double value,
            const std::vector<std::uint8_t>& bytes = {},
            bool readBack = false) {
        asio::io_context& io = *ctx.io;
        MyProt::Gateway::ProtocolGateway& gateway = *ctx.gateway;
        const std::shared_ptr<std::vector<MyProt::Core::ProtocolConfig> >&
            protosPtr = ctx.protos;
        const std::shared_ptr<std::vector<MyProt::Core::DeviceConfig> >&
            devicesPtr = ctx.devices;
        const std::shared_ptr<std::vector<MyProt::Core::TagDefinition> >&
            tagsPtr = ctx.tags;
        typedef MyProt::Core::VoidExpected WriteResult;
        auto promise = std::make_shared<std::promise<WriteResult> >();
        std::future<WriteResult> future = promise->get_future();

        io.post([protosPtr, devicesPtr, tagsPtr, &gateway,
                 tagName, value, bytes, readBack, promise]() {
            // 1. 查标签
            const MyProt::Core::TagDefinition* tag = 0;
            for (size_t i = 0; i < tagsPtr->size(); ++i) {
                if ((*tagsPtr)[i].name == tagName) {
                    tag = &(*tagsPtr)[i];
                    break;
                }
            }
            if (!tag) {
                promise->set_value(MyProt::Core::Unexpected(
                    MyProt::Core::Error::Code::TagNotFound,
                    "标签未找到: " + tagName));
                return;
            }
            // 2. 查设备 → 协议名 + 请求超时 (2026-08-24 收敛: 唯一配置点)
            std::string protoName;
            int requestTimeoutMs = 3000;
            for (size_t i = 0; i < devicesPtr->size(); ++i) {
                if ((*devicesPtr)[i].id == tag->deviceId) {
                    protoName = (*devicesPtr)[i].protocol;
                    requestTimeoutMs = (*devicesPtr)[i].requestTimeoutMs;
                    break;
                }
            }
            if (protoName.empty()) {
                promise->set_value(MyProt::Core::Unexpected(
                    MyProt::Core::Error::Code::DeviceNotFound,
                    "设备未找到: " + tag->deviceId));
                return;
            }
            // 3. 协议快照 (shared_ptr<const>) — GOC/WriteOnce/WriteBytes 异步期间
            //    热重载可能替换 *protosPtr, 按值拷贝一次后经 shared_ptr 保活
            bool foundProto = false;
            MyProt::Core::ProtocolConfig protoCopy;
            for (size_t i = 0; i < protosPtr->size(); ++i) {
                if ((*protosPtr)[i].protocolName == protoName) {
                    protoCopy = (*protosPtr)[i];
                    foundProto = true;
                    break;
                }
            }
            if (!foundProto) {
                promise->set_value(MyProt::Core::Unexpected(
                    MyProt::Core::Error::Code::ProtocolNotFound,
                    "协议未找到: " + protoName));
                return;
            }
            const std::shared_ptr<const MyProt::Core::ProtocolConfig> proto =
                std::make_shared<const MyProt::Core::ProtocolConfig>(
                    std::move(protoCopy));
            // 4. 获取通道 (已连接 fast-path 同步回调; 首次异步连接)
            //    tag 按值拷贝: GOC 异步连接期间热重载可能替换 *tagsPtr,
            //    指针将指向被换出的旧向量元素 (悬垂)。
            //    bytes 按值拷贝: 同上, lambda 异步期间仍需保活。
                        // 写标签 (direction=write) — 写 op 取 tag.operation,
            //    不参与轮询。
                        // 标签级写能力 — direction=read 标签在标签上声明
            //    writeOperation (标量写) / writeBytesOperation (变长写);
                        //    写声明点唯一: 标签 (协议级写字段不属于 Schema)。
            const MyProt::Core::TagDefinition tagCopy = *tag;
            const bool isWriteTag = (tagCopy.direction == "write");
            const bool hasTagWrite = isWriteTag || !tagCopy.writeOperation.empty();
            std::string writeOpName;
            if (hasTagWrite) {
                writeOpName = isWriteTag ? tagCopy.operation
                                         : tagCopy.writeOperation;
                if (proto->operations.find(writeOpName) == proto->operations.end()) {
                    promise->set_value(MyProt::Core::Unexpected(
                        MyProt::Core::Error::Code::ConfigError,
                        "写操作未找到: " + writeOpName
                            + " (protocol: " + protoName
                            + ", 标签 " + tagCopy.name + ")"));
                    return;
                }
            }
            // 写请求有效标签: variables ∪ writeVariables (同名键覆盖),
            // 仅作用于写请求 — 读路径仍用 tagCopy 原变量表。
            MyProt::Core::TagDefinition tagWrite = tagCopy;
            for (std::unordered_map<std::string, std::uint32_t>::const_iterator
                    iv = tagCopy.writeVariables.begin();
                    iv != tagCopy.writeVariables.end(); ++iv) {
                tagWrite.variables[iv->first] = iv->second;
            }
            const std::vector<std::uint8_t> bytesCopy = bytes;
            gateway.GetChannelManager().GetOrCreateChannel(
                tagCopy.deviceId,
                [&gateway, tagCopy, tagWrite, proto, isWriteTag, hasTagWrite,
                 writeOpName, value,
                 bytesCopy, readBack, requestTimeoutMs, promise, tagsPtr](
                        MyProt::Core::Expected<
                            MyProt::Gateway::ConnectResult> cr) {
                    if (!cr.has_value()) {
                        Core::metrics::CounterInc(
                            Core::metrics::kWriteFailuresTotal,
                            Core::metrics::Device(tagCopy.deviceId));
                        promise->set_value(MyProt::Core::VoidExpected(
                            MyProt::Core::UnexpectedType{cr.error()}));
                        return;
                    }
                    // 5. 构建写请求 → 发送 → echo 校验
                                        // 统一的完成回调 (指标 + promise 置值)
                    auto onWriteDone =
                        [promise, tagCopy](MyProt::Core::VoidExpected wr) {
                            Core::metrics::CounterInc(
                                wr.has_value()
                                    ? Core::metrics::kWritesTotal
                                    : Core::metrics::kWriteFailuresTotal,
                                Core::metrics::Device(tagCopy.deviceId));
                            promise->set_value(std::move(wr));
                        };

                                        // 5a. 组装写后读回校验 (readBack=true 时)
                    //     读 op 取 readBackTag (缺省写标签自身) 的读语义,
                    //     期望字节 = 写入字节 / 按 finalType 编码的 value
                    std::shared_ptr<const MyProt::Gateway::WriteBackCheck>
                        backCheck;
                    if (readBack) {
                        MyProt::Gateway::WriteBackCheck check;
                        std::string cbErr;
                        if (!BuildWriteBackCheck(*tagsPtr, *proto, tagCopy,
                                value, bytesCopy, check, cbErr)) {
                            Core::metrics::CounterInc(
                                Core::metrics::kWriteFailuresTotal,
                                Core::metrics::Device(tagCopy.deviceId));
                            promise->set_value(MyProt::Core::Unexpected(
                                MyProt::Core::Error::Code::ReadBackMismatch,
                                "读回校验组装失败: " + cbErr));
                            return;
                        }
                        backCheck = std::make_shared<
                            MyProt::Gateway::WriteBackCheck>(std::move(check));
                    }

                    // 5b. 变长路径: bytes 直传; 可写标签 Float/Double 标量
                    //     → IEEE754 大端字节 → {writeVariable:raw} 注入;
                                        //     写 op 模板以 {writeVariable:raw} 消费时, 整数
                    //     标量同样按 finalType 宽度编码走 BuildBytes 派生链
                    //     (S7 WriteVar — WriteOnce 不派生 PDULength/DataLen 等)
                                        //     经 OpNeedsBytePath 判定 (不能只查 raw), 模板引用
                    //     任何仅 BuildBytes 注入的派生量 (PDULength/DataLength/
                    //     DataBits/DataLen/RegisterCount/ByteCount) 也强制走
                    //     BuildBytes 路径, 修复 "模板变量未提供 (PDULength)".
                    bool opNeedsBytePath = false;
                    if (hasTagWrite) {
                        std::unordered_map<std::string,
                            MyProt::Core::OperationConfig>::const_iterator wOpIt =
                            proto->operations.find(writeOpName);
                        if (wOpIt != proto->operations.end()) {
                            opNeedsBytePath = OpNeedsBytePath(
                                *proto, wOpIt->second, tagCopy.writeVariable);
                        }
                    }
                    if (!bytesCopy.empty()
                            || (hasTagWrite
                                && (opNeedsBytePath
                                    || IsFloatFinalType(tagCopy.finalType)))) {
                        std::vector<std::uint8_t> payload = bytesCopy;
                        if (bytesCopy.empty()) {
                            std::string encErr;
                            if (!EncodeFinalTypeToBytes(tagCopy.finalType, value,
                                MyProt::Engine::ResponseParser::ResolveByteOrder(
                                    *proto, tagCopy.byteOrder),
                                payload, encErr)) {
                                Core::metrics::CounterInc(
                                    Core::metrics::kWriteFailuresTotal,
                                    Core::metrics::Device(tagCopy.deviceId));
                                promise->set_value(MyProt::Core::Unexpected(
                                    MyProt::Core::Error::Code::ConfigError,
                                    encErr));
                                return;
                            }
                        }
                        std::unordered_map<std::string, std::string> rawVars;
                        rawVars[tagCopy.writeVariable.empty()
                            ? Core::kDefaultWriteValueVariable : tagCopy.writeVariable] =
                            BytesToHexSpace(payload);
                                                // 变长写 op 取标签级 writeBytesOperation (写标签取
                        //   tag.operation); 仅声明了标量写的标签回退其 writeOperation
                        //   (模板 {Name:Xn} 同样可消费编码后的载荷)。
                        const std::string writeBytesOp = isWriteTag
                            ? writeOpName
                            : (tagCopy.writeBytesOperation.empty()
                                ? tagCopy.writeOperation
                                : tagCopy.writeBytesOperation);
                        // 标签未声明任何变长写能力 → fail-fast
                        if (writeBytesOp.empty()) {
                            Core::metrics::CounterInc(
                                Core::metrics::kWriteFailuresTotal,
                                Core::metrics::Device(tagCopy.deviceId));
                            promise->set_value(MyProt::Core::Unexpected(
                                MyProt::Core::Error::Code::ConfigError,
                                "标签 " + tagCopy.name
                                    + " 未声明 writeBytesOperation, 变长写不可用 (protocol: "
                                    + proto->protocolName + "); 请在标签上声明"
                                    + " writeBytesOperation (标签级写能力)"));
                            return;
                        }
                        // 注: 原 P1 D busStrand 透传已撤回(2026-08-29,见 ADR-0011 §3.2)
                        gateway.GetTagReader().WriteBytes(
                            tagWrite, proto, writeBytesOp, rawVars,
                            backCheck,
                            *cr.value().channel,
                            requestTimeoutMs,
                            onWriteDone);
                        return;
                    }

                                        // 5c. 可写标签整数标量: 按 finalType 校验范围
                    if (hasTagWrite) {
                        std::string rangeErr;
                        if (!CheckIntegerWriteValue(tagCopy.finalType,
                                value, rangeErr)) {
                            Core::metrics::CounterInc(
                                Core::metrics::kWriteFailuresTotal,
                                Core::metrics::Device(tagCopy.deviceId));
                            promise->set_value(MyProt::Core::Unexpected(
                                MyProt::Core::Error::Code::ConfigError,
                                rangeErr));
                            return;
                        }
                        gateway.GetTagReader().WriteOnce(
                            tagWrite, proto, writeOpName,
                            static_cast<std::uint32_t>(
                                static_cast<std::int64_t>(value)),
                            tagCopy.writeVariable.empty()
                                ? Core::kDefaultWriteValueVariable : tagCopy.writeVariable,
                            backCheck,
                            *cr.value().channel,
                            requestTimeoutMs,
                            onWriteDone);
                        return;
                    }

                    // 5d. 未声明写能力的只读标签 → fail-fast
                                        //     (写声明点唯一: 标签; 协议级 writeOperation 兜底不存在
                    //     收敛于标签层 — 只读标签的语义就是"写 API 必须拒绝")
                    Core::metrics::CounterInc(
                        Core::metrics::kWriteFailuresTotal,
                        Core::metrics::Device(tagCopy.deviceId));
                    promise->set_value(MyProt::Core::Unexpected(
                        MyProt::Core::Error::Code::ConfigError,
                        "标签 " + tagCopy.name
                            + " 未声明 writeOperation, 标量写不可用 (protocol: "
                            + proto->protocolName + "); 请在标签上声明"
                            + " writeOperation (读写标签) 或 writeBytesOperation"
                            + ", 或配置写标签 (direction=write)"));
                });
        });

        // wait_for 上限防 io 线程卡死拖挂 WebApi 线程
        if (future.wait_for(std::chrono::seconds(10))
                != std::future_status::ready) {
            return MyProt::Core::Unexpected(
                MyProt::Core::Error::Code::Timeout, "写操作等待超时");
        }
        return future.get();
    }

        // ── POST /api/data/write — 单寄存器写端点 ──
    std::pair<int, std::string> HandleWriteApi(
            const AppContext& ctx, const HttpRequest& req) {
        const std::string& method = req.method;
        const std::string& path = req.path;
        const std::string& body = req.body;
        typedef std::pair<int, std::string> Resp;

        if (path.rfind("/api/data/write", 0) != 0)
            return Resp(404, "{\"error\":\"unknown data endpoint\"}");
        if (method != "POST")
            return Resp(405, "{\"error\":\"method not allowed\"}");

        nlohmann::json doc;
        try { doc = nlohmann::json::parse(body); }
        catch (const nlohmann::json::parse_error&) {
            return Resp(400, "{\"error\":\"JSON 解析失败\"}");
        } catch (const std::exception&) {
            return Resp(400, "{\"error\":\"内部错误\"}");
        }
        nlohmann::json::const_iterator jt = doc.find("tag");
        nlohmann::json::const_iterator jv = doc.find("value");
        nlohmann::json::const_iterator jb = doc.find("bytes");
        if (jt == doc.end() || !jt->is_string()) {
            return Resp(400,
                "{\"error\":\"body 须为 {\\\"tag\\\":\\\"N\\\",\\\"value\\\":N} "
                "或 {\\\"tag\\\":\\\"N\\\",\\\"bytes\\\":\\\"...\\\"}\"}");
        }
        // 互斥校验: value 与 bytes 二选一, 不可同时出现
        const bool hasValue = (jv != doc.end());
        const bool hasBytes = (jb != doc.end());
        if (hasValue && hasBytes) {
            return Resp(400,
                "{\"error\":\"value 与 bytes 互斥, 仅可指定其一\"}");
        }
        if (!hasValue && !hasBytes) {
            return Resp(400,
                "{\"error\":\"body 须含 value (标量) 或 bytes (变长) 字段\"}");
        }

        // value 为 double — 浮点按 finalType 编码 (Float/Double → IEEE754),
        // 整型范围由标签 finalType 决定 (WriteViaGateway 的 CheckIntegerWriteValue;
        // 64 位整型引导走 bytes 路径避免精度损失)。
        double value = 0.0;
        std::vector<std::uint8_t> bytes;
        // P1 B: 读回标志 (默认 false; true 时写完成后立即读同地址校验)
        bool readBack = false;
        nlohmann::json::const_iterator jr = doc.find("readBack");
        if (jr != doc.end()) {
            if (!jr->is_boolean()) {
                return Resp(400, "{\"error\":\"readBack 须为 bool\"}");
            }
            readBack = jr->get<bool>();
        }
        if (hasValue) {
            // 仅做与标签无关的入参校验 (须为数值); 标签相关校验 (可写性 / 值域 /
            // 目标类型编码) 全部下沉到 io 线程写路径完成 — 见 WriteViaGateway。
            // 修复: 旧实现此处扫描 ctx.tags 判定"是否可写标签", 而该向量由热重载
            //       在 io 线程整体赋值 (*tagsPtr = ...), 跨线程读 vector = 数据竞争 (UB)。
            if (!jv->is_number()) {
                return Resp(400, "{\"error\":\"value 须为数值\"}");
            }
            value = jv->get<double>();
        } else {
            // bytes 路径 (P1 A): 接受 hex 字符串 ("01 0A 0B") 或
            // base64 字符串 ("Awo=") 两种形态; 解析失败即 400
            if (!jb->is_string()) {
                return Resp(400, "{\"error\":\"bytes 须为字符串\"}");
            }
            const std::string b = jb->get<std::string>();
            // 先按 hex 解析 (含空格)
            std::string compact;
            compact.reserve(b.size());
            for (size_t i = 0; i < b.size(); ++i) {
                if (b[i] != ' ' && b[i] != '\t') compact += b[i];
            }
            if (compact.size() % 2 == 0 && !compact.empty()) {
                bool isHex = true;
                for (size_t i = 0; i < compact.size(); ++i) {
                    const char c = compact[i];
                    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                            || (c >= 'A' && c <= 'F'))) {
                        isHex = false; break;
                    }
                }
                if (isHex) {
                    bytes.reserve(compact.size() / 2);
                    for (size_t i = 0; i < compact.size(); i += 2) {
                        char buf[3] = { compact[i], compact[i + 1], 0 };
                        bytes.push_back(static_cast<std::uint8_t>(
                            std::strtoul(buf, nullptr, 16)));
                    }
                }
            }
            if (bytes.empty()) {
                return Resp(400, "{\"error\":\"bytes 须为 hex 字符串 (偶数位)\"}");
            }
        }

        const MyProt::Core::VoidExpected wr = WriteViaGateway(
            ctx, jt->get<std::string>(), value, bytes, readBack);
        if (!wr.has_value()) {
            nlohmann::json err;
                        // Error.context (变量名/设备名等定位信息) 一并回传 —
            //   丢失会让 "模板变量未提供" 这类错误无从定位.
            err["error"] = (wr.error().message.empty()
                                ? "write failed" : wr.error().message)
                + (wr.error().context.empty()
                       ? std::string() : " [" + wr.error().context + "]");
            // 503 触发条件 (P1 B + P1 C): read-back 不一致 或 写互斥 Busy
            // (与 write 502 区分: 503 = 服务端暂不可用, 客户端可重试)
            const int httpCode =
                (wr.error().code == MyProt::Core::Error::Code::ReadBackMismatch
                 || wr.error().code == MyProt::Core::Error::Code::Busy)
                ? 503 : 502;
            return Resp(httpCode, err.dump());
        }
        return Resp(200, "{\"ok\":true}");
    }

}} // namespace MyProt::App

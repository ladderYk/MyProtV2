// src/Gateway/src/WriteService.cpp
// 写路径实现（P2-2）— 自 App/DataApi.cpp 下沉，行为与原实现逐字节一致：
//   查标签/设备/协议快照 → 可写性判定 → 标量/变长编码 → WriteOnce/WriteBytes
//   → 可选写后读回校验；promise+future 同步等待，上限 10s 防 io 线程卡死拖挂。
#include "MyProt/Gateway/WriteService.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Engine/RequestBuilder.hpp"
#include "MyProt/Engine/ResponseParser.hpp"
#include "MyProt/Gateway/TagGrouper.hpp"

namespace MyProt { namespace Gateway {

WriteService::WriteService(asio::io_context& io, ProtocolGateway& gateway,
                           Core::RuntimeSnapshotHolder& snapshot)
    : _io(io), _gateway(gateway), _snapshot(snapshot) {}

// ── 写标签辅助 ──

    // finalType 是否浮点族 (Float/Double → IEEE754 大端字节编码)
    static bool IsFloatFinalType(const std::string& ft) {
        return ft == "Float" || ft == "Double";
    }

        // 判断写操作模板是否以 {Name:raw} 消费写值 —
    // S7 WriteVar 等变长模板的整数标量也须走 WriteBytes (BuildBytes) 路径:
    // WriteOnce 标量路径不派生 PDULength/DataLen/DataLength/DataBits,
    // 模板构建即报 "模板变量未提供 (PDULength)"。
    static bool OpConsumesRawValue(const MyProt::Core::OperationConfig& op,
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
    static std::string ExtractPlaceholderName(const std::string& part) {
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
    bool WriteService::OpNeedsBytePath(const MyProt::Core::ProtocolConfig& proto,
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
    static std::string BytesToHexSpace(const std::vector<std::uint8_t>& b) {
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
    bool WriteService::EncodeFinalTypeToBytes(const std::string& ft, double v,
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
    bool WriteService::CheckIntegerWriteValue(const std::string& ft, double v,
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
    bool WriteService::BuildWriteBackCheck(
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
    MyProt::Core::VoidExpected WriteService::Write(const WriteRequest& req) {
        const std::string& tagName = req.tagName;
        const double value = req.value;
        const std::vector<std::uint8_t>& bytes = req.bytes;
        const bool readBack = req.readBack;
        asio::io_context& io = _io;
        MyProt::Gateway::ProtocolGateway& gateway = _gateway;
        // 取当前运行时快照并保活到异步写完成 — 热重载只做整代替换,
        // 已捕获的旧快照在 lambda 结束前保持有效 (无悬垂/无跨线程改写)
        const MyProt::Core::RuntimeSnapshotPtr snapshot = _snapshot.Get();
        if (!snapshot) {
            return MyProt::Core::Unexpected(MyProt::Core::Error::Code::ConfigError,
                                            "运行时配置快照尚未装配");
        }
        typedef MyProt::Core::VoidExpected WriteResult;
        auto promise = std::make_shared<std::promise<WriteResult> >();
        std::future<WriteResult> future = promise->get_future();

        ProtocolGateway* gatewayPtr = &gateway;
        io.post([snapshot, gatewayPtr,
                 tagName, value, bytes, readBack, promise]() {
            ProtocolGateway& gateway = *gatewayPtr;
            const std::vector<MyProt::Core::TagDefinition>& tags = snapshot->tags;
            const std::vector<MyProt::Core::DeviceConfig>& devices = snapshot->devices;
            const std::vector<MyProt::Core::ProtocolConfig>& protos = snapshot->protocols;
            // 1. 查标签
            const MyProt::Core::TagDefinition* tag = 0;
            for (size_t i = 0; i < tags.size(); ++i) {
                if (tags[i].name == tagName) {
                    tag = &tags[i];
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
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].id == tag->deviceId) {
                    protoName = devices[i].protocol;
                    requestTimeoutMs = devices[i].requestTimeoutMs;
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
            //    热重载会整代替换快照, 此处按值拷贝一次后经 shared_ptr 保活
            bool foundProto = false;
            MyProt::Core::ProtocolConfig protoCopy;
            for (size_t i = 0; i < protos.size(); ++i) {
                if (protos[i].protocolName == protoName) {
                    protoCopy = protos[i];
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
            //    tag 按值拷贝: GOC 异步连接期间热重载会整代替换快照, 旧快照
            //    由 snapshot 的 shared_ptr 保活 (无悬垂)。
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
                 bytesCopy, readBack, requestTimeoutMs, promise, snapshot](
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
                        if (!BuildWriteBackCheck(snapshot->tags, *proto, tagCopy,
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

}} // namespace MyProt::Gateway

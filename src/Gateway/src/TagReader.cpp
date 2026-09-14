// src/Gateway/src/TagReader.cpp — 标签读取器实现
// 单读/批量读/写共用同一套 RequestBuilder → SendReceive → ResponseParser 管线
#include "MyProt/Gateway/TagReader.hpp"
#include "MyProt/Gateway/TagGrouper.hpp"
#include "MyProt/Engine/AutoComputeProvider.hpp"  // 派生长度求值 (ResolveDerivedLength)
#include <chrono>

namespace MyProt { namespace Gateway {

namespace {

/// 模板布局与派生长度注入的唯一实现在 Engine (TemplateLayout.kRawMarker /
/// RequestBuilder::BuildTemplateLayout / InjectDerivedLengthVariables);
/// Gateway 与 Service 试算校验共用同一实现 (ADR-0012 §1.2) — 本层不再自持该实现.

/// 每次 Build/BuildBytes 前同步 autoCompute 规则到 _autoProvider.
///   - 空串 = 无规则, RequestBuilder 走兜底路径 (Next 原子自增).
///   - 非空 = 整段重声明: 先清旧规则, 再解析新规则; 计数器保留 (Next 跨调用).
///   协议切换时避免旧协议规则残留. 内容相同的重复同步由 provider 内部快路径
///   短路 (免 JSON 解析) — 稳态轮询下每请求调用的实际开销为一次字符串比较.
///   _autoProvider 的 rules 受其 Impl 内部 mutex 保护, 可跨线程调用.
void SyncAutoComputeRules(Engine::AutoComputeProvider& autoProvider,
                          const Core::ProtocolConfig& protocol) {
    const std::string ac = Engine::RequestBuilder::CollectAutoComputeJson(protocol);
    if (!ac.empty()) {
        autoProvider.DeclareJson(ac);
    }
}
/// 读回 Build 用 — 协议 autoComputeJson 已在 BuildWriteBackCheck
/// 快照到 check->autoComputeJson, 直接传字符串即可.
void SyncAutoComputeRules(Engine::AutoComputeProvider& autoProvider,
                          const std::string& autoComputeJson) {
    if (!autoComputeJson.empty()) {
        autoProvider.DeclareJson(autoComputeJson);
    }
}

void SyncAutoComputeRules(Engine::AutoComputeProvider& autoProvider,
                          const Core::ProtocolConfig& protocol,
                          const Core::OperationConfig& op) {
    const std::string merged = MergeOpAutoComputeJson(protocol, op);
    if (!merged.empty()) {
        autoProvider.DeclareJson(merged);
    }
}

// 从 op.inputs 中提取 source=static 且有值的条目, 转成 ctx.variables 形式.
//   auto 与无值的 static 不进 ctx.variables (auto 走 AutoComputeProvider).
std::unordered_map<std::string, uint32_t> OpStaticVariables(
    const Core::OperationConfig& op) {
    std::unordered_map<std::string, uint32_t> out;
    for (auto it = op.inputs.begin(); it != op.inputs.end(); ++it) {
        const Core::VariableConfig& v = it->second;
        if (v.isStatic() && v.value.has_value()) {
            out[it->first] = v.value.value();
        }
    }
    return out;
}

// InjectDerivedLengthVariables 实现在 Engine::RequestBuilder (见文件头注释).
//   此处仅保留 op 级 autoIncrement 预解析 (ResolveAutoIncrementParameters).

// 参数层预解析 — 把声明的 strategy=autoIncrement 变量在渲染前解析进参数表,
//   使模板取值节点 {Name:Xn} 对 autoIncrement 也"纯查表" (帧感知 frameSlice/expr/crc 除外,
//   它们读取已渲染帧, 只能原地渲染期求值). 计数器跨调用保留 (Next 语义不变); 标签显式提供的同名变量优先.
void ResolveAutoIncrementParameters(
        std::unordered_map<std::string, uint32_t>& variables,
        const std::unordered_map<std::string, Core::VariableConfig>& protocolVars,
        const std::unordered_map<std::string, Core::VariableConfig>& opVars,
        Engine::AutoComputeProvider& autoProvider) {
    auto inject = [&](const std::unordered_map<std::string, Core::VariableConfig>& vars) {
        for (auto it = vars.begin(); it != vars.end(); ++it) {
            const Core::VariableConfig& v = it->second;
            if (!v.isAuto() || v.strategy != "autoIncrement") continue;
            if (variables.find(it->first) != variables.end()) continue;  // 显式优先
            if (!autoProvider.IsAutoIncrement(it->first)) continue;
            Engine::BuildContext ctx;
            ctx.variables = &variables;
            variables[it->first] = static_cast<uint32_t>(
                autoProvider.Resolve(it->first, 2, ctx) & 0xFFFFFFFFull);
        }
    };
    inject(protocolVars);
    inject(opVars);
}

} // namespace

/// hex 字符串 (可能含空格/制表符分隔) → 实际字节数
/// WriteViaGateway 编码时每字节 3 字符 "XX ", 直接 (size+1)/2 会高估 50%
std::size_t HexByteCount(const std::string& s) {
    std::size_t hexChars = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') ++hexChars;
    }
    return hexChars / 2;
}

/// 协议级 + op 级 autoCompute 合并 (公开 API, 对应 TagReader.hpp 声明).
/// 实现在 Engine::RequestBuilder::MergeOpAutoComputeJson — 与 Service
///   试算校验共用同一实现 (禁止在 Gateway/Service 各写一份, 否则 derivedLength 处理会分叉)。
std::string MergeOpAutoComputeJson(const Core::ProtocolConfig& protocol,
                                   const Core::OperationConfig& op) {
    return Engine::RequestBuilder::MergeOpAutoComputeJson(protocol, op);
}

TagReader::TagReader() {}

// ── 批量读取 ──

void TagReader::ReadBatch(const MergedRequest& merged,
                          const std::vector<Core::TagDefinition>& tags,
                          const Core::ProtocolConfig& protocol,
                          Transport::IChannel& channel,
                          int requestTimeoutMs,
                          BatchHandler handler) {
    // 1. 查找操作配置
    auto opIt = protocol.operations.find(merged.operation);
    if (opIt == protocol.operations.end()) {
        // 操作未找到 → 为每个 tag 生成错误值
        // 注意: merged.tagIndices 是全局标签表索引, tags 是本批次子集
        // (两者按下标一一对应, 不可用 tagIndices 下标 tags — 越界)
        std::vector<Core::TagValue> results;
        for (size_t i = 0; i < merged.tagIndices.size() && i < tags.size(); ++i) {
            Core::TagValue tv;
            tv.tagName = tags[i].name;
            tv.deviceId = merged.deviceId;
            tv.quality = Core::QualityCode::Bad;
            tv.lastError = Core::Error::Make(Core::Error::Code::TagNotFound,
                "操作未找到: " + merged.operation);
            results.push_back(std::move(tv));
        }
        handler(std::move(results));
        return;
    }
    const Core::OperationConfig op = opIt->second;  // 按值持有 — 回调异步执行

    // 2. 构建合并请求的变量表
        //    协议 static 变量为基, 首个 tag 的 variables 覆盖同名键
        //    中间插入 op.inputs 静态覆盖 (协议 < op < 标签)
        //    注入 ByteCount (跨协议字节单位); 不注入 RegisterCount (协议族单位, 由 outputs 派生).
    //          Modbus 协议 JSON 在 op.outputs 声明 outputs.RegisterCount = derivedLength({ByteCount} / 2);
    //          S7 协议 JSON 直接在 op.outputs 声明 outputs.ByteCount (或直接读 tag.variables.ByteCount).
    //          引擎零协议族假设, RegisterCount 完全由协议 JSON 通过 derivedLength 派生.
    std::unordered_map<std::string, uint32_t> variables =
        Engine::RequestBuilder::CollectStaticVariables(protocol);
    if (!tags.empty()) {
        variables = Engine::RequestBuilder::MergeVariables(
            variables, OpStaticVariables(op), tags[0].variables);
    } else {
        // 没有标签时也合 op (例如纯写 op 没绑定 tag) — 优先级仍是 op > 协议
        variables = Engine::RequestBuilder::MergeVariables(
            variables, OpStaticVariables(op),
            std::unordered_map<std::string, uint32_t>{});
    }
    variables[Core::StartByteAddressVariableName()] = merged.startByteAddress;
        variables[Core::ByteCountVariableName()]      = merged.byteCount;  // 跨协议字节单位
        // 位偏移为标签一等字段 — 注入 expr 作用域 (供协议 outputs derivedLength 引用)
    if (!tags.empty() && tags[0].bitOffset >= 0) {
        variables[Core::kBitOffsetExprVariable] = static_cast<uint32_t>(tags[0].bitOffset);
    }

        // ReadBatch 路径调用 InjectDerivedLengthVariables (与 WriteBytes 路径一致).
    //   协议 op.outputs 中 derivedLength 声明 (如 Modbus outputs.RegisterCount = {ByteCount}/2)
    //   在 Build 之前注入 variables; 标签显式提供的同名变量保持优先 (Inject 内部已实现).
    //   读 op 无 raw 字节流 → totalBytes = 0; 仅标量 derivedLength (依赖 {Name} 变量) 求值.
    {
        const Engine::TemplateLayout layout =
            Engine::RequestBuilder::BuildTemplateLayout(op.requestTemplate);
        Engine::RequestBuilder::InjectDerivedLengthVariables(
            variables, protocol.outputs, op.outputs, /*totalBytes=*/0, layout);
    }

    // 3. 构建请求字节 (同步; 失败即为所有 tag 标记 Bad)
        //    同步协议 autoCompute 规则 (空 = 不动)
        //    同时合并 op.inputs 中 source=auto 的覆盖项
    SyncAutoComputeRules(_autoProvider, protocol, op);
        // 参数层预解析 — autoIncrement 渲染前进参数表 (帧感知 frameSlice/expr/crc 除外)
    ResolveAutoIncrementParameters(variables, protocol.inputs, op.inputs, _autoProvider);
    auto buildResult = _requestBuilder.Build(op, variables, protocol.varAliasMap, _autoProvider);
    if (!buildResult.has_value()) {
        std::vector<Core::TagValue> results;
        for (size_t i = 0; i < merged.tagIndices.size() && i < tags.size(); ++i) {
            Core::TagValue tv;
            tv.tagName = tags[i].name;
            tv.deviceId = merged.deviceId;
            tv.quality = Core::QualityCode::Bad;
            tv.lastError = buildResult.error();
            results.push_back(std::move(tv));
        }
        handler(std::move(results));
        return;
    }

    // 4. 逐标签字节序裁决 — 批量响应中不同标签字节序可能不同 (如 CDAB 浮点与
    //    默认大端整型混在同一合并批), 不可用首标签 byteOrder 整批统一, 否则
    //    非首标签的 byteOrder 覆盖被吞掉 → 高低字颠倒。
    std::vector<Core::ByteOrder> tagOrders;
    tagOrders.reserve(tags.size());
    for (size_t i = 0; i < tags.size(); ++i) {
        tagOrders.push_back(
            Engine::ResponseParser::ResolveByteOrder(protocol, tags[i].byteOrder));
    }

    // 5. 异步发送/接收
    //    注意: 回调与 async_write 均为异步完成 — tags/request 必须按值捕获保活
    //    (batchTags 是 PollBatchChain 的局部向量, 按引用捕获将在返回后悬垂)
    //    超时: 统一取 device.requestTimeoutMs (2026-08-24 收敛为唯一配置点)
    auto framing = std::make_shared<Core::FramingConfig>(protocol.framing);
    auto requestShared = std::make_shared<Core::Bytes>(std::move(buildResult.value()));
    channel.SendReceive(*requestShared, framing,
                        std::chrono::milliseconds(requestTimeoutMs),
        [this, merged, tags, op, tagOrders, handler, requestShared](
            Core::Expected<Core::Bytes> result) {

            std::vector<Core::TagValue> results;
            results.reserve(merged.tagIndices.size());

            if (!result.has_value()) {
                // 通信失败 → 所有 tag 标记 Bad
                for (size_t i = 0; i < merged.tagIndices.size() && i < tags.size(); ++i) {
                    Core::TagValue tv;
                    tv.tagName = tags[i].name;
                    tv.deviceId = merged.deviceId;
                    tv.quality = Core::QualityCode::Bad;
                    tv.lastError = result.error();
                    results.push_back(std::move(tv));
                }
                handler(std::move(results));
                return;
            }

            // 6. 为每个 tag 解析响应
            Core::ByteView response(result.value());
            for (size_t i = 0; i < merged.tagIndices.size() && i < tags.size(); ++i) {
                const auto& tag = tags[i];

                // 调整 dataStartIndex: 批量响应中该 tag 的字节偏移.
                                //   字节地址差 = tag.StartAddress (协议 JSON derivedLength 派生前已是字节)
                //   - merged.startByteAddress; 跨协议普适, 无"寄存器=2字节"假设.
                //   例: Modbus 读 HR0..HR3 (合并 4 寄存器 = 8 字节), 协议 JSON 里:
                //     tag.StartAddress (协议 derivedLength 派生前为寄存器号 × 2 后的字节地址) →
                //     merged.startByteAddress (协议 derivedLength 派生前为最小字节地址).
                uint32_t tagAddr = TagGrouper::GetStartAddress(tag);
                int offset = static_cast<int>(tagAddr - merged.startByteAddress);

                // 创建调整后的解析配置
                Core::ResponseParserConfig adjustedConfig = op.responseParser;
                adjustedConfig.dataStartIndex = op.responseParser.dataStartIndex + offset;

                auto parseResult = _responseParser.Parse(
                    response, adjustedConfig, tag, tagOrders[i]);

                if (parseResult.has_value()) {
                    auto& tv = parseResult.value();
                    tv.tagName = tag.name;
                    tv.deviceId = merged.deviceId;
                    results.push_back(std::move(tv));
                } else {
                    Core::TagValue tv;
                    tv.tagName = tag.name;
                    tv.deviceId = merged.deviceId;
                    tv.quality = Core::QualityCode::Bad;
                    tv.lastError = parseResult.error();
                    results.push_back(std::move(tv));
                }
            }

            handler(std::move(results));
        });
}

// ── 单寄存器写 (/write) ──

void TagReader::WriteOnce(const Core::TagDefinition& tag,
                          Core::ProtocolConfig protocol,
                          const std::string& writeOperation,
                          std::uint32_t value,
                          const std::string& valueVariable,
                          const std::shared_ptr<const WriteBackCheck>& backCheck,
                          Transport::IChannel& channel,
                          int requestTimeoutMs,
                          WriteHandler handler) {
    // P1 C (ADR-0011 §3.1): per-device 写互斥. 入口抢位; 失败 → Busy (HTTP 503).
    // map 缺省构造 atomic<bool>(false) 已保证新设备位初始 false.
    // 注: 原 P1 D busStrand + runImpl lambda + strand.post 已撤回
    //   (2026-08-29,见 ADR-0011 §3.2 撤回说明). 单 io_context 部署下,
    //   写链天然串行, 不需要额外 strand 间接层. P1 C 写互斥仍生效, 因
    //   其粒度为单 device 写 vs 写, 不依赖 io 线程模型.
    std::atomic<bool>& writeSlot = _writingInFlight[tag.deviceId];
    if (writeSlot.exchange(true)) {
        handler(Core::Unexpected(Core::Error::Code::Busy,
            "device 写互斥失败: 已有在途写",
            "device=" + tag.deviceId));
        return;
    }

    // 0. 查找写操作配置
    auto opIt = protocol.operations.find(writeOperation);
    if (opIt == protocol.operations.end()) {
        writeSlot.store(false);  // 失败路径释放在途位
        handler(Core::Unexpected(Core::Error::Code::ConfigError,
            "写操作未找到: " + writeOperation
                + " (protocol: " + protocol.protocolName + ")"));
        return;
    }
    // 按值持有 — 回调异步执行, 不能引用局部 opIt
    const Core::OperationConfig op = opIt->second;

    // 2. 变量表 = 协议 defaultVariables + op.inputs.static + tag.variables + StartByteAddress + {valueVariable}
        //    协议 static 变量为基, 标签级 variables 覆盖同名键
        //    注入变量名由调用方指定 (写标签用 tag.writeVariable)
        //    插入 op 级 static 覆盖 (协议 < op < 标签)
        //    注入跨协议字节单位 StartByteAddress; 协议族地址 (如 Modbus StartAddress
    //           寄存器号) 由下方 InjectDerivedLengthVariables 按协议 JSON outputs 派生.
    std::unordered_map<std::string, uint32_t> variables =
        Engine::RequestBuilder::MergeVariables(
            Engine::RequestBuilder::CollectStaticVariables(protocol),
            OpStaticVariables(op),
            tag.variables);
    variables[Core::StartByteAddressVariableName()] = TagGrouper::GetStartAddress(tag);
    variables[valueVariable.empty() ? Core::kDefaultWriteValueVariable : valueVariable] = value;
        // 位偏移为标签一等字段 — 注入 expr 作用域 (供位寻址派生, 如 Modbus FC05/FC15)
    if (tag.bitOffset >= 0) {
        variables[Core::kBitOffsetExprVariable] = static_cast<uint32_t>(tag.bitOffset);
    }

        // 写路径派生长度注入 (与 ReadBatch/WriteBytes 一致).
    //   协议 JSON outputs.StartAddress = StartByteAddress/2 派生协议族地址;
    //   标签显式提供的同名变量保持优先 (Inject 内部已实现).
    {
        const Engine::TemplateLayout layout =
            Engine::RequestBuilder::BuildTemplateLayout(op.requestTemplate);
        Engine::RequestBuilder::InjectDerivedLengthVariables(
            variables, protocol.outputs, op.outputs, /*totalBytes=*/0, layout);
    }

    // 3. 构建请求字节 (同步; 失败即快速返回)
        //    同步协议 autoCompute 规则 (空 = 不动)
        //    同时合并 op.inputs 中 source=auto 的覆盖项
    SyncAutoComputeRules(_autoProvider, protocol, op);
    auto buildResult = _requestBuilder.Build(op, variables, protocol.varAliasMap, _autoProvider);
    if (!buildResult.has_value()) {
        writeSlot.store(false);  // 失败路径释放在途位
        handler(Core::VoidExpected(Core::UnexpectedType{buildResult.error()}));
        return;
    }

    // 4. 异步发送 — tag/op/请求字节均按值或 shared_ptr 保活
    //    (同 ReadBatch 教训: 异步完成期间局部变量不可按引用捕获)
    //    超时: 统一取 device.requestTimeoutMs (2026-08-24 收敛为唯一配置点)
    //    P1 C (ADR-0011 §3.1): 单一 callback 路径 (成功/失败/超时 终态)
    //    释放在途位. wrapped 是 SendReceive 终态的**唯一**出口; 内部
    //    read-back 子链也走 wrapped 触发释放.
    auto framing = std::make_shared<Core::FramingConfig>(protocol.framing);
    auto requestShared = std::make_shared<Core::Bytes>(std::move(buildResult.value()));
    auto releaseSlot = [&writeSlot](Core::VoidExpected r) {
        writeSlot.store(false);
        return r;
    };
    auto writeRelease = [handler, releaseSlot](Core::VoidExpected r) {
        handler(releaseSlot(std::move(r)));
    };
    channel.SendReceive(*requestShared, framing,
                        std::chrono::milliseconds(requestTimeoutMs),
        [this, op, framing, &channel, requestTimeoutMs,
         backCheck, writeRelease, requestShared, &writeSlot, protocol](
            Core::Expected<Core::Bytes> result) {

            if (!result.has_value()) {
                writeRelease(Core::VoidExpected(Core::UnexpectedType{result.error()}));
                return;
            }
            // 写应答只校验 validCondition (echo 帧), 不解析数据区
            Core::ByteView response(result.value());
            if (!Engine::ResponseParser::CheckCondition(
                    response, op.responseParser.validCondition)) {
                writeRelease(Core::Unexpected(Core::Error::Code::WriteFailed,
                    "写应答校验失败: " + op.responseParser.validCondition));
                return;
            }
                        // read-back: 读 op/期望字节由调用方组装,
            // 经 RunReadBack 统一执行 — 不再硬编码 Modbus FC03 布局,
            // 也不依赖协议恰好存在 "ReadHoldingRegisters" 模板。
            if (!backCheck) {
                writeRelease(Core::VoidExpected());
                return;
            }
            RunReadBack(channel, framing, requestTimeoutMs, backCheck, protocol.varAliasMap, writeRelease);
        });
}

// ── 变长字节写 (P1 A) ──
// 复用 WriteOnce 管线; 唯一差异: RequestBuilder 走 BuildBytes 路径, 把
// variableBytesHex 表中的 hex 字符串解析为字节流, 由 {Name:raw} 占位符
// 直插到请求帧中 (如 Modbus FC16 的数据区、IEC104 ASDU 等变长载荷)。
void TagReader::WriteBytes(const Core::TagDefinition& tag,
                           Core::ProtocolConfig protocol,
                           const std::string& writeOperation,
                           const std::unordered_map<std::string, std::string>& variableBytesHex,
                           const std::shared_ptr<const WriteBackCheck>& backCheck,
                           Transport::IChannel& channel,
                           int requestTimeoutMs,
                           WriteHandler handler) {
    // P1 C (ADR-0011 §3.1): per-device 写互斥 — 同 device 写与写互斥
    // 注: 原 P1 D busStrand + runImpl lambda + strand.post 已撤回
    //   (2026-08-29,见 ADR-0011 §3.2 撤回说明)
    std::atomic<bool>& writeSlot = _writingInFlight[tag.deviceId];
    if (writeSlot.exchange(true)) {
        handler(Core::Unexpected(Core::Error::Code::Busy,
            "device 写互斥失败: 已有在途写",
            "device=" + tag.deviceId));
        return;
    }

    // 0. 查找写操作配置
    auto opIt = protocol.operations.find(writeOperation);
    if (opIt == protocol.operations.end()) {
        writeSlot.store(false);
        handler(Core::Unexpected(Core::Error::Code::ConfigError,
            "写操作未找到: " + writeOperation
                + " (protocol: " + protocol.protocolName + ")"));
        return;
    }
    // 按值持有 — 回调异步执行, 不能引用局部 opIt
    const Core::OperationConfig op = opIt->second;

    // 2. 标量变量表 = 协议 defaultVariables + op.inputs.static + tag.variables + StartByteAddress;
    //    变长变量表直接采用调用方注入
        //    协议 static 变量为基, 标签级 variables 覆盖同名键
        //    插入 op 级 static 覆盖 (协议 < op < 标签)
        //    注入跨协议字节单位 StartByteAddress (协议族地址由 outputs 派生, 见下方 Inject).
    std::unordered_map<std::string, uint32_t> variables =
        Engine::RequestBuilder::MergeVariables(
            Engine::RequestBuilder::CollectStaticVariables(protocol),
            OpStaticVariables(op),
            tag.variables);
    variables[Core::StartByteAddressVariableName()] = TagGrouper::GetStartAddress(tag);
        // 位偏移为标签一等字段 — 注入 expr 作用域 (供位寻址派生, 如 Modbus FC05/FC15)
    if (tag.bitOffset >= 0) {
        variables[Core::kBitOffsetExprVariable] = static_cast<uint32_t>(tag.bitOffset);
    }

        // 变长写派生长度注入 (配置驱动; 实现在 Engine::RequestBuilder)
    //   扫描 protocol.outputs ∪ op.outputs 中 source=auto strategy=derivedLength 的声明,
    //   按 expr + 载荷字节数自动算值注入 (参数层预解析); 标签显式提供的同名变量保持优先.
        //   expr 支持 {Frame:fixed} / {Name:offset} 模板结构原语 (ADR-0012 §1.1).
    {
        std::size_t totalBytes = 0;
        for (const auto& kv : variableBytesHex) {
            totalBytes += HexByteCount(kv.second);
        }
        const Engine::TemplateLayout layout =
            Engine::RequestBuilder::BuildTemplateLayout(op.requestTemplate);
        Engine::RequestBuilder::InjectDerivedLengthVariables(
            variables, protocol.outputs, op.outputs, totalBytes, layout);
    }

    // 3. 构建请求字节 (P1 A 路径: BuildBytes)
        //    同步协议 autoCompute 规则 (空 = 不动)
        //    同时合并 op.inputs 中 source=auto 的覆盖项
    SyncAutoComputeRules(_autoProvider, protocol, op);
    ResolveAutoIncrementParameters(variables, protocol.inputs, op.inputs, _autoProvider);
    auto buildResult = _requestBuilder.BuildBytes(
        op, variables, variableBytesHex, protocol.varAliasMap, _autoProvider);
    if (!buildResult.has_value()) {
        writeSlot.store(false);
        handler(Core::VoidExpected(Core::UnexpectedType{buildResult.error()}));
        return;
    }

    // 4. 异步发送 — tag/op/请求字节均按值或 shared_ptr 保活
    //    P1 C (ADR-0011 §3.1): 单一 callback 路径释放在途位
    auto framing = std::make_shared<Core::FramingConfig>(protocol.framing);
    auto requestShared = std::make_shared<Core::Bytes>(std::move(buildResult.value()));
    auto releaseSlot = [&writeSlot](Core::VoidExpected r) {
        writeSlot.store(false);
        return r;
    };
    auto writeRelease = [handler, releaseSlot](Core::VoidExpected r) {
        handler(releaseSlot(std::move(r)));
    };
    channel.SendReceive(*requestShared, framing,
                        std::chrono::milliseconds(requestTimeoutMs),
        [this, op, writeRelease, requestShared, framing, &channel,
         requestTimeoutMs, backCheck, protocol](
            Core::Expected<Core::Bytes> result) {

            if (!result.has_value()) {
                writeRelease(Core::VoidExpected(Core::UnexpectedType{result.error()}));
                return;
            }
            // 写应答只校验 validCondition (echo 帧), 不解析数据区
            Core::ByteView response(result.value());
            if (!Engine::ResponseParser::CheckCondition(
                    response, op.responseParser.validCondition)) {
                writeRelease(Core::Unexpected(Core::Error::Code::WriteFailed,
                    "写应答校验失败: " + op.responseParser.validCondition));
                return;
            }
                        // read-back: 与 WriteOnce 一致 — 读 op/期望
                        // 字节由调用方组装 (复用写 op 模板会发出错误请求):
            // 构建读请求 (Modbus 下直接构建失败, S7 下发出第二个写请求
            // 造成"假成功"), 且只校验长度未做字节比较。
            if (!backCheck) {
                writeRelease(Core::VoidExpected());
                return;
            }
            RunReadBack(channel, framing, requestTimeoutMs, backCheck, protocol.varAliasMap, writeRelease);
        });
}

// ── 写后读回统一执行体 ──
// 按 check->readOp 构建读请求 → 发送 → 以 readOp.responseParser.dataStartIndex
// 为数据区起点, 逐字节比较 check->expectedBytes。终态恰好回调 done 一次。
void TagReader::RunReadBack(Transport::IChannel& channel,
                            const std::shared_ptr<const Core::FramingConfig>& framing,
                            int requestTimeoutMs,
                            const std::shared_ptr<const WriteBackCheck>& check,
                            const std::unordered_map<std::string, std::string>& varAliasMap,
                            const WriteHandler& done) {
    const int dataStart = check->readOp.responseParser.dataStartIndex;
    if (dataStart <= 0) {
        done(Core::Unexpected(Core::Error::Code::ReadBackMismatch,
            "读回模板 dataStartIndex 缺失 (须 > 0)",
            "tag=" + check->tagLabel));
        return;
    }
        // 读回 Build 同步 autoCompute 规则 (BuildWriteBackCheck 已从源 protocol 快照)
    SyncAutoComputeRules(_autoProvider, check->autoComputeJson);
    auto buildResult = _requestBuilder.Build(check->readOp, check->readVars,
                                             varAliasMap, _autoProvider);
    if (!buildResult.has_value()) {
        done(Core::VoidExpected(Core::UnexpectedType{buildResult.error()}));
        return;
    }
    // 读回请求/framing 按值或 shared_ptr 保活 (异步回调)
    auto readReq = std::make_shared<Core::Bytes>(std::move(buildResult.value()));
    auto readFraming = framing;
    channel.SendReceive(*readReq, readFraming,
                        std::chrono::milliseconds(requestTimeoutMs),
        [done, check, dataStart, readReq, readFraming](
            Core::Expected<Core::Bytes> readResult) {
            if (!readResult.has_value()) {
                done(Core::VoidExpected(Core::UnexpectedType{readResult.error()}));
                return;
            }
            const Core::Bytes& rb = readResult.value();
            const std::size_t need = static_cast<std::size_t>(dataStart)
                                   + check->expectedBytes.size();
            if (rb.size() < need) {
                done(Core::Unexpected(Core::Error::Code::ReadBackMismatch,
                    "read-back 响应过短: 期望 ≥ " + std::to_string(need)
                        + " 字节, 实际 " + std::to_string(rb.size()),
                    "tag=" + check->tagLabel));
                return;
            }
            // 逐字节比较: 数据区 [dataStart, dataStart+len) == expectedBytes
            for (std::size_t i = 0; i < check->expectedBytes.size(); ++i) {
                if (rb[static_cast<std::size_t>(dataStart) + i]
                        != check->expectedBytes[i]) {
                    // 拼接整段 hex 便于诊断 (expected vs got)
                    static const char* kHex = "0123456789ABCDEF";
                    std::string expHex, gotHex;
                    for (std::size_t j = 0; j < check->expectedBytes.size(); ++j) {
                        expHex.push_back(kHex[(check->expectedBytes[j] >> 4) & 0xF]);
                        expHex.push_back(kHex[check->expectedBytes[j] & 0xF]);
                        expHex.push_back(' ');
                        gotHex.push_back(kHex[(rb[static_cast<std::size_t>(dataStart) + j] >> 4) & 0xF]);
                        gotHex.push_back(kHex[rb[static_cast<std::size_t>(dataStart) + j] & 0xF]);
                        gotHex.push_back(' ');
                    }
                    done(Core::Unexpected(Core::Error::Code::ReadBackMismatch,
                        "read-back 不一致: expected=[" + expHex + "] got=[" + gotHex + "]",
                        "tag=" + check->tagLabel));
                    return;
                }
            }
            done(Core::VoidExpected());
        });
}

}} // namespace MyProt::Gateway

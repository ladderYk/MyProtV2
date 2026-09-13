// src/App/RuntimeGlue.cpp — 生产/E2E 两端共享的装配与扩展路由胶水层
// 自 main.cpp 迁出 (2026-08-25): 崩溃取证、sim/data/write 扩展端点、
// ApplyRuntimeSync 运行时装配。本 cpp 同时编入 MyProt.App 与 MyProt.E2E。
// 2026-08-26: 运行时状态收敛为 AppContext 单参传递 — 各函数开头将 ctx 成员
// 绑定回原名局部引用, 业务正文与 lambda 捕获保持不变。
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
#include "MyProt/Core/ServerConfig.hpp"  // v1.1 增: LoadedConfig.server
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/ByteOrder.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Core/Metrics.hpp"
#include "MyProt/Gateway/ProtocolGateway.hpp"
#include "MyProt/Gateway/TagReader.hpp"     // v1.6: WriteBackCheck
#include "MyProt/Gateway/TagGrouper.hpp"    // v1.6: GetStartAddress (读回变量表)
#include "MyProt/Polling/PollingEngine.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "MyProt/Engine/ResponseParser.hpp"   // v1.28: 写路径 byteOrder 裁决共用 (读写一致)
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "RuntimeGlue.hpp"
#include <nlohmann/json.hpp>

namespace MyProt { namespace App {
std::atomic<bool> g_running(true);
void OnSignal(int) { g_running.store(false); }

// 未捕获异常诊断: 打印 what() 后按 MSVC abort 语义终止
void OnTerminate() {
    std::exception_ptr ep = std::current_exception();
    if (ep) {
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) {
            std::cerr << "[FATAL] 未捕获异常: " << e.what() << std::endl;
        }
        catch (...) {
            std::cerr << "[FATAL] 未捕获未知异常" << std::endl;
        }
    } else {
        std::cerr << "[FATAL] terminate (无活动异常)" << std::endl;
    }
    std::cerr.flush();
    std::abort();
}

// ── 硬崩溃取证: SIGABRT / SEH 未处理异常 + dbghelp 栈回溯 ──
void PrintCurrentStack(WORD skip) {
    void* frames[32] = {0};
    const WORD n = ::CaptureStackBackTrace(skip, 32, frames, NULL);
    HANDLE proc = ::GetCurrentProcess();
    for (WORD i = 0; i < n; ++i) {
        char symBuf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (::SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]),
                          &disp, sym)) {
            DWORD lineDisp = 0;
            IMAGEHLP_LINE64 line;
            memset(&line, 0, sizeof(line));
            line.SizeOfStruct = sizeof(line);
            if (::SymGetLineFromAddr64(proc,
                    reinterpret_cast<DWORD64>(frames[i]), &lineDisp, &line)) {
                std::cerr << "  #" << i << " " << sym->Name
                          << " [" << line.FileName << ":" << line.LineNumber
                          << "]" << std::endl;
            } else {
                std::cerr << "  #" << i << " " << sym->Name
                          << " +0x" << std::hex << disp << std::dec << std::endl;
            }
        } else {
            std::cerr << "  #" << i << " 0x" << std::hex
                      << reinterpret_cast<DWORD64>(frames[i])
                      << std::dec << std::endl;
        }
    }
    std::cerr.flush();
}

#ifdef _DEBUG
// Debug CRT 断言钩子: 报告时上下文完好, 是抓栈的最佳时机
int OnCrtReport(int reportType, char* message, int* /*returnValue*/) {
    if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR) {
        std::cerr << "[CRT-REPORT] " << (message ? message : "(null)")
                  << std::endl;
        PrintCurrentStack(1);
    }
    return 0; // 继续默认处理流程
}
#endif
void PrintStackTrace(CONTEXT* ctx) {
    HANDLE proc = ::GetCurrentProcess();
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    if (!::SymInitialize(proc, NULL, TRUE)) return;

    CONTEXT c = *ctx;
    ::STACKFRAME64 frame;
    memset(&frame, 0, sizeof(frame));
#ifdef _WIN64
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = c.Rip;
    frame.AddrFrame.Offset = c.Rbp;
    frame.AddrStack.Offset = c.Rsp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = c.Eip;
    frame.AddrFrame.Offset = c.Ebp;
    frame.AddrStack.Offset = c.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 32; ++i) {
        if (!::StackWalk64(machine, proc, ::GetCurrentThread(), &frame, &c,
                           NULL, ::SymFunctionTableAccess64,
                           ::SymGetModuleBase64, NULL)) break;
        if (frame.AddrPC.Offset == 0) break;
        char symBuf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (::SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym)) {
            std::cerr << "  #" << i << " " << sym->Name
                      << " +0x" << std::hex << disp << std::dec << std::endl;
        } else {
            std::cerr << "  #" << i << " 0x"
                      << std::hex << frame.AddrPC.Offset << std::dec << std::endl;
        }
    }
    ::SymCleanup(proc);
}

LONG WINAPI OnUnhandledExc(EXCEPTION_POINTERS* ep) {
    std::cerr << "[FATAL] 硬件异常 code=0x" << std::hex
              << ep->ExceptionRecord->ExceptionCode << std::dec
              << " addr=" << ep->ExceptionRecord->ExceptionAddress << std::endl;
    std::cerr.flush();
    if (ep->ContextRecord != 0) PrintStackTrace(ep->ContextRecord);
    return EXCEPTION_CONTINUE_SEARCH;
}

void OnSigAbort(int) {
    std::cerr << "[FATAL] SIGABRT: abort() 被调用, 栈回溯:" << std::endl;
    PrintCurrentStack(1);
}
/// URL query 参数提取 ("/api/x?start=3&count=8" → key=start 返回 "3")
std::string QueryParam(const std::string& path, const std::string& key) {
    const std::size_t q = path.find('?');
    if (q == std::string::npos) return std::string();
    std::size_t pos = q + 1;
    while (pos < path.size()) {
        const std::size_t amp = path.find('&', pos);
        const std::string kv = path.substr(
            pos, amp == std::string::npos ? std::string::npos : amp - pos);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return std::string();
}

// ══ Phase 2: /api/sim/* 仿真数据面 (UI 运行时操控仿真器数据区) ══
//   GET  /api/sim/status                            仿真器清单 [{protocol,listenPort,registerCount}]
//   GET  /api/sim/registers?proto=&start=&count=    读寄存器 (唯一仿真器时 proto 可省)
//   POST /api/sim/registers   {"start":N,"values":[v..]} (protocol 字段可选)

MyProt::Simulation::SimulationServer* PickSim(
        const SimServerMap& sims, const std::string& proto,
        int& status, std::string& errJson) {
    if (!proto.empty()) {
        SimServerMap::const_iterator it = sims.find(proto);
        if (it == sims.end()) {
            status = 404;
            errJson = "{\"error\":" +
                      nlohmann::json("无此仿真协议: " + proto).dump() + "}";
            return 0;
        }
        return it->second;
    }
    if (sims.empty()) {
        status = 404; errJson = "{\"error\":\"无运行中的仿真器\"}";
        return 0;
    }
    if (sims.size() > 1) {
        status = 400; errJson = "{\"error\":\"存在多个仿真协议, 请以 ?proto= 指定\"}";
        return 0;
    }
    return sims.begin()->second;
}

std::pair<int, std::string> HandleSimApi(const HttpRequest& req,
                                         const SimServerMap& sims) {
    const std::string& method = req.method;
    const std::string& path = req.path;
    const std::string& body = req.body;
    typedef std::pair<int, std::string> Resp;

    // ── GET /api/sim/status ──
    if (path.rfind("/api/sim/status", 0) == 0) {
        if (method != "GET") {
            return Resp(405, "{\"error\":\"method not allowed\"}");
        }
        nlohmann::json arr = nlohmann::json::array();
        for (SimServerMap::const_iterator it = sims.begin();
                it != sims.end(); ++it) {
            nlohmann::json o;
            o["protocol"] = it->first;
            o["listenPort"] = it->second->ListenPort();
            o["registerCount"] = static_cast<int>(it->second->Store().Size());
            arr.push_back(o);
        }
        nlohmann::json out;
        out["simulators"] = arr;
        return Resp(200, out.dump());
    }

    // ── /api/sim/registers (GET=读 / POST=写) ──
    if (path.rfind("/api/sim/registers", 0) == 0) {
        if (method == "GET") {
            int st = 0; std::string ejs;
            MyProt::Simulation::SimulationServer* sim =
                PickSim(sims, QueryParam(path, "proto"), st, ejs);
            if (!sim) return Resp(st, ejs);

            const long start = std::strtol(
                QueryParam(path, "start").c_str(), 0, 10);
            long count = std::strtol(
                QueryParam(path, "count").c_str(), 0, 10);
            if (count <= 0) count = 16;                    // 缺省读 16 个
            if (start < 0 || start > 65535 || count > 65536 ||
                    start + count > 65536) {
                return Resp(400, "{\"error\":\"地址范围越界 [0,65535]\"}");
            }
            const std::vector<std::uint8_t> data = sim->Store().ReadRegisters(
                static_cast<std::uint16_t>(start),
                static_cast<std::uint16_t>(count));
            if (data.empty()) {
                return Resp(400, "{\"error\":\"读取超出数据区\"}");
            }
            nlohmann::json vals = nlohmann::json::array();
            for (std::size_t i = 0; i + 1 < data.size(); i += 2) {
                vals.push_back(static_cast<int>(
                    (static_cast<int>(data[i]) << 8) | data[i + 1]));
            }
            nlohmann::json out;
            out["start"] = static_cast<int>(start);
            out["values"] = vals;
            return Resp(200, out.dump());
        }

        if (method == "POST") {
            nlohmann::json doc;
            try { doc = nlohmann::json::parse(body); }
            catch (const nlohmann::json::parse_error&) {
                return Resp(400, "{\"error\":\"JSON 解析失败\"}");
            } catch (const std::exception&) {
                return Resp(400, "{\"error\":\"内部错误\"}");
            }
            nlohmann::json::const_iterator jp = doc.find("protocol");
            int st = 0; std::string ejs;
            MyProt::Simulation::SimulationServer* sim =
                PickSim(sims, jp != doc.end() && jp->is_string()
                            ? jp->get<std::string>() : std::string(),
                        st, ejs);
            if (!sim) return Resp(st, ejs);

            nlohmann::json::const_iterator js = doc.find("start");
            nlohmann::json::const_iterator jv = doc.find("values");
            if (js == doc.end() || !js->is_number_integer()
                    || jv == doc.end() || !jv->is_array() || jv->empty()) {
                return Resp(400,
                    "{\"error\":\"body 须为 {\\\"start\\\":N,\\\"values\\\":[..]}\"}");
            }
            const int start = js->get<int>();
            if (start < 0 || start > 65535) {
                return Resp(400, "{\"error\":\"start 越界\"}");
            }
            std::vector<std::uint8_t> data;
            data.reserve(jv->size() * 2);
            // 注: 遍历用迭代器而非 operator[] — v140 Debug 下 iter_impl::operator[] 不稳定
            for (nlohmann::json::const_iterator vi = jv->begin();
                    vi != jv->end(); ++vi) {
                if (!vi->is_number()) {
                    return Resp(400, "{\"error\":\"values 须为整数数组\"}");
                }
                const int v = vi->get<int>();
                if (v < 0 || v > 65535) {
                    return Resp(400, "{\"error\":\"寄存器值须在 [0,65535]\"}");
                }
                data.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
                data.push_back(static_cast<std::uint8_t>(v & 0xFF));
            }
            if (!sim->Store().WriteRegisters(
                    static_cast<std::uint16_t>(start), data)) {
                return Resp(400, "{\"error\":\"写入超出数据区\"}");
            }
            return Resp(200, "{\"ok\":true}");
        }

        return Resp(405, "{\"error\":\"method not allowed\"}");
    }

    return Resp(404, "{\"error\":\"unknown sim endpoint\"}");
}

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

    // ── v1.6 写标签辅助 ──

    // finalType 是否浮点族 (Float/Double → IEEE754 大端字节编码)
    bool IsFloatFinalType(const std::string& ft) {
        return ft == "Float" || ft == "Double";
    }

    // v1.7 修正: 判断写操作模板是否以 {Name:raw} 消费写值 —
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

    // v1.8 增: 写 op 是否需要走 BuildBytes 路径.
    // 条件: 模板以 {Name:raw} 消费写值, 或模板引用了「依赖载荷长度」的派生量
    //   (expr 含 {name:len} — 仅 BuildBytes 有 payload 上下文可求值, 如 PDULength 族;
    //   单条件判定取代 v1.7 单一 raw 检查, 修复 S7 模板 {PDULength:X4} 在
    //   不含 {WriteValue:raw} 时被误判走 Build 标量路径的问题).
    // v1.32 收紧: 原实现把「模板引用任何派生量」都判为字节路径 — 但仅由静态量
    //   派生的值 (如 Modbus {StartAddress} = StartByteAddress/2) 在 WriteOnce 的
    //   InjectDerivedLengthVariables 同样可解析; 误判使整数标量写被推入 BuildBytes,
    //   而该路径的 uint32 变量表不含 WriteValue (载荷以 hex 进 variableBytesHex),
    //   {WriteValue:X4} 渲染报 "模板变量未提供 [WriteValue]" — FC06 标量写全数失败
    //   (E2E 17-3 / 20a, 自 v1.8 起即为红).
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

    // v1.6: 组装写后读回校验参数.
    // 读标签选择: 写标签 readBackTag 优先; 未配置时用写标签自身 —
    //   legacy 场景下写目标即读标签, 其 operation 本就是读 op,
    //   以其自身读语义原样重读 (取代 v1.5 的 "ReadHoldingRegisters"
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
        // v1.25: 注入跨协议字节单位; 协议族地址 (如 Modbus StartAddress 寄存器号)
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
        // v1.10 改: 读回也是 op 级 (readOp), 需要合并 op.inputs 中 source=auto
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

    // ── /api/data/write 写路径同步封装 (v1.5; v1.6 写标签扩展) ──
    // WebApi 线程 → io.post 全链 (查标签/设备/协议 → GOC → WriteOnce/WriteBytes),
    // 与轮询回调在 io 线程串行 — 消除跨线程 socket 操作与配置向量竞争。
    // promise 用 shared_ptr 保活: 超时返回后 lambda 可能仍在 io 队列中执行。
    // bytes 路径 (P1 A 销账项): 非空时走 WriteBytes 变长写, 透传 {Name:raw} 注入表。
    // v1.6 写标签 (tag.direction == "write"):
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
            // 3. 按值拷贝协议 — GOC/WriteOnce 异步期间热重载可能替换 *protosPtr
            bool foundProto = false;
            MyProt::Core::ProtocolConfig proto;
            for (size_t i = 0; i < protosPtr->size(); ++i) {
                if ((*protosPtr)[i].protocolName == protoName) {
                    proto = (*protosPtr)[i];
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
            // 4. 获取通道 (已连接 fast-path 同步回调; 首次异步连接)
            //    tag 按值拷贝: GOC 异步连接期间热重载可能替换 *tagsPtr,
            //    指针将指向被换出的旧向量元素 (悬垂)。
            //    bytes 按值拷贝: 同上, lambda 异步期间仍需保活。
            // v1.6: 写标签 (direction=write) — 写 op 取 tag.operation,
            //    不参与轮询。
            // v1.7/v1.32: 标签级写能力 — direction=read 标签在标签上声明
            //    writeOperation (标量写) / writeBytesOperation (变长写);
            //    写声明点收敛于标签 (协议级字段已移除)。
            const MyProt::Core::TagDefinition tagCopy = *tag;
            const bool isWriteTag = (tagCopy.direction == "write");
            const bool hasTagWrite = isWriteTag || !tagCopy.writeOperation.empty();
            std::string writeOpName;
            if (hasTagWrite) {
                writeOpName = isWriteTag ? tagCopy.operation
                                         : tagCopy.writeOperation;
                if (proto.operations.find(writeOpName) == proto.operations.end()) {
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
                    // v1.6: 统一的完成回调 (指标 + promise 置值)
                    auto onWriteDone =
                        [promise, tagCopy](MyProt::Core::VoidExpected wr) {
                            Core::metrics::CounterInc(
                                wr.has_value()
                                    ? Core::metrics::kWritesTotal
                                    : Core::metrics::kWriteFailuresTotal,
                                Core::metrics::Device(tagCopy.deviceId));
                            promise->set_value(std::move(wr));
                        };

                    // 5a. v1.6: 组装写后读回校验 (readBack=true 时)
                    //     读 op 取 readBackTag (缺省写标签自身) 的读语义,
                    //     期望字节 = 写入字节 / 按 finalType 编码的 value
                    std::shared_ptr<const MyProt::Gateway::WriteBackCheck>
                        backCheck;
                    if (readBack) {
                        MyProt::Gateway::WriteBackCheck check;
                        std::string cbErr;
                        if (!BuildWriteBackCheck(*tagsPtr, proto, tagCopy,
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
                    //     v1.7: 写 op 模板以 {writeVariable:raw} 消费时, 整数
                    //     标量同样按 finalType 宽度编码走 BuildBytes 派生链
                    //     (S7 WriteVar — WriteOnce 不派生 PDULength/DataLen 等)
                    //     v1.8: 用 OpNeedsBytePath 取代单一 raw 检查, 模板引用
                    //     任何仅 BuildBytes 注入的派生量 (PDULength/DataLength/
                    //     DataBits/DataLen/RegisterCount/ByteCount) 也强制走
                    //     BuildBytes 路径, 修复 "模板变量未提供 (PDULength)".
                    bool opNeedsBytePath = false;
                    if (hasTagWrite) {
                        std::unordered_map<std::string,
                            MyProt::Core::OperationConfig>::const_iterator wOpIt =
                            proto.operations.find(writeOpName);
                        if (wOpIt != proto.operations.end()) {
                            opNeedsBytePath = OpNeedsBytePath(
                                proto, wOpIt->second, tagCopy.writeVariable);
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
                                    proto, tagCopy.byteOrder),
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
                        // v1.32: 变长写 op 取标签级 writeBytesOperation (写标签取
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
                                    + proto.protocolName + "); 请在标签上声明"
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

                    // 5c. v1.6/v1.7 可写标签整数标量: 按 finalType 校验范围
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
                    //     (v1.32: 协议级 writeOperation 兜底已移除, 写声明点
                    //     收敛于标签层 — 只读标签的语义就是"写 API 必须拒绝")
                    Core::metrics::CounterInc(
                        Core::metrics::kWriteFailuresTotal,
                        Core::metrics::Device(tagCopy.deviceId));
                    promise->set_value(MyProt::Core::Unexpected(
                        MyProt::Core::Error::Code::ConfigError,
                        "标签 " + tagCopy.name
                            + " 未声明 writeOperation, 标量写不可用 (protocol: "
                            + proto.protocolName + "); 请在标签上声明"
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

    // ── POST /api/data/write — 单寄存器写端点 (v1.5) ──
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
            // v1.32: Error.context (变量名/设备名等定位信息) 一并回传 —
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

    // ── reload 运行时联动 — 生产与 E2E 共用同一代码路径 (首次装配亦走此入口) ──
    // 重新加载配置目录 → 全停重建仿真器 (simulation.listenPort > 0) → 清空实时快照
    // → post 到 io 线程重装配轮询引擎 (tags/devices 变更生效)。
    // 新配置非法时保持旧运行态并返回错误 (配合 ConfigStore.Save 的自动回滚)。
    // 线程说明: 引擎重装配投递到 io 线程与 in-flight 回调串行执行; 旧代组复活
    // 由 PollingEngine 代际号防护 (ScheduleNext/OnTimerFire 校验 generation)。
    // 注意: SimulationServer 持有 ProtocolConfig 引用, 故新配置必须存入调用方
    // 持有的 protoStore (函数局部 fresh 返回即销毁)。
    MyProt::Core::VoidExpected ApplyRuntimeSync(
            const std::string& configDir,
            AppContext& ctx,
            std::ostream* log) {
        asio::io_context& io = *ctx.io;
        const std::shared_ptr<std::vector<MyProt::Core::ProtocolConfig> >&
            protosPtr = ctx.protos;
        const std::shared_ptr<std::vector<MyProt::Core::DeviceConfig> >&
            devicesPtr = ctx.devices;
        const std::shared_ptr<std::vector<MyProt::Core::TagDefinition> >&
            tagsPtr = ctx.tags;
        MyProt::Gateway::ProtocolGateway& gateway = *ctx.gateway;
        MyProt::Polling::PollingEngine& engine = *ctx.engine;
        const EngineStarter& startEngine = ctx.startEngine;
        SimServerMap& sims = *ctx.sims;
        std::vector<std::unique_ptr<MyProt::Simulation::SimulationServer> >&
            owners = *ctx.simOwners;
        MyProt::Polling::LatestValueStore& latest = *ctx.latest;
        std::vector<MyProt::Core::ProtocolConfig>& protoStore = *ctx.protoStore;
        const MyProt::Core::Expected<MyProt::Service::LoadedConfig> fresh =
            MyProt::Service::ConfigDirectoryLoader::Load(configDir, MyProt::Core::kSupportedSchemaVersion);
        if (!fresh.has_value()) {
            return MyProt::Core::Unexpected(MyProt::Core::Error::Code::ConfigError,
                                            "重载失败: " + fresh.error().message);
        }

        // 1. 全停旧仿真器 (先停再换数据, 析构顺序安全)
        for (size_t i = 0; i < owners.size(); ++i) owners[i]->Stop();
        owners.clear();
        sims.clear();

        // 2. 新配置落入调用方持有的容器, 再据此重建仿真器
        // v1.1 改: simulation 段从 ProtocolConfig 移至 ServerConfig (server.json).
        // 多协议共享同一 ServerConfig.simulation — listenPort 即"是否启用的开关".
        protoStore = fresh.value().protocols;
        const MyProt::Core::ServerConfig& svr = fresh.value().server;
        if (svr.simulation.listenPort > 0) {
            if (protoStore.empty()) {
                if (log)
                    *log << "[WARN] server.json 启用仿真但无协议文件 (protocols/ 为空), 跳过" << std::endl;
            } else {
                // 取 0 号协议作为仿真器语法依据 (operation 名空间);
                // server.json 的 operations map 决定行为, 协议层只贡献"名字".
                const MyProt::Core::ProtocolConfig& p = protoStore[0];
                std::unique_ptr<MyProt::Simulation::SimulationServer> sim(
                    new MyProt::Simulation::SimulationServer(p, svr.simulation));
                std::string err;
                if (sim->Start(err)) {
                    if (log)
                        *log << "[OK] 仿真器 " << p.protocolName
                             << " 已监听 tcp://127.0.0.1:" << svr.simulation.listenPort
                             << std::endl;
                    sims[p.protocolName] = sim.get();
                    owners.push_back(std::move(sim));
                } else if (log) {
                    *log << "[WARN] 仿真器 " << p.protocolName
                         << " 启动失败: " << err << std::endl;
                }
            }
        }

        // 3. 清空实时快照 — 旧标签集数据作废, 由引擎按新一轮轮询重新积累
        latest.Clear();

        // 4. 引擎侧热重载 — post 到 io 线程 (与 in-flight 轮询回调串行):
        //    停轮询 → 切换 lookup/factory 数据源 → 关通道+重建设备表 → 重启引擎。
        //    代际号保证 Stop→Start 连续切换时旧组 in-flight 不复活。
        const MyProt::Service::LoadedConfig cfg = fresh.value();
        io.post([protosPtr, devicesPtr, tagsPtr, &gateway, &engine,
                 &startEngine, cfg, log]() {
            engine.Stop();
            *protosPtr = cfg.protocols;
            *devicesPtr = cfg.root.devices;
            *tagsPtr = cfg.root.tags;
            gateway.Shutdown();
            gateway.GetChannelManager().ResetDevices(cfg.root.devices);
            startEngine(cfg.root.tags, cfg.root.devices,
                        cfg.root.resilience);
            if (log)
                *log << "[OK] 轮询引擎已热重载 (" << cfg.root.tags.size()
                     << " 标签/" << cfg.root.devices.size() << " 设备)"
                     << std::endl;
        });
        return MyProt::Core::VoidExpected();
    }


// ══ 对外稳定 API — 崩溃取证 / 停止信号 (生产入口 main.cpp 调用) ══

void InstallCrashDiagnostics() {
    std::set_terminate(&OnTerminate);
    ::signal(SIGABRT, &OnSigAbort);
    ::SetUnhandledExceptionFilter(&OnUnhandledExc);
#ifdef _DEBUG
    // Debug CRT 断言/错误默认走调试器+弹窗, 重定向到 stderr 以便取证
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportHook(&OnCrtReport);
#endif
    // 预热符号引擎 (abort 上下文中初始化不可靠)
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    ::SymInitialize(::GetCurrentProcess(), NULL, TRUE);
}

bool IsRunning() { return g_running.load(); }

void InstallStopSignals() {
    ::signal(SIGINT, OnSignal);
    ::signal(SIGTERM, OnSignal);
}

}} // namespace MyProt::App

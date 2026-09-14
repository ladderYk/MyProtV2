// src/App/SimApi.cpp — /api/sim/* 仿真数据面 (方案1-S2 自 RuntimeGlue.cpp 逐字节搬移, 零行为变更)
// 内容: PickSim (仿真器选择/报错) + HandleSimApi (status 清单 / registers 读(GET) 写(POST))。

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
#include "MyProt/Engine/ResponseParser.hpp"   // 写路径 byteOrder 裁决共用 (读写
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Simulation/SimulationServer.hpp"
#include "RuntimeGlue.hpp"
#include "AppSignals.hpp"
#include "CrashDiagnostics.hpp"
#include <nlohmann/json.hpp>

namespace MyProt { namespace App {
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

}} // namespace MyProt::App

// src/App/DataApi.cpp — /api/data/* 实时数据面 + 写端点 HTTP 适配
// P2-2 后职责收敛：本文件只做 HTTP 语义（body 解析 / 字节串解码 / 状态码映射），
// 写路径本体（可写性判定 / 编解码 / 读回校验 / 异步写）已下沉 Gateway::WriteService。

#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Value.hpp"
#include "MyProt/Gateway/WriteService.hpp"
#include "MyProt/Polling/LatestValueStore.hpp"
#include "RuntimeGlue.hpp"

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

// ── POST /api/data/write — 单寄存器写端点 (HTTP 适配) ──
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
    // 整型范围由标签 finalType 决定 (WriteService 的 CheckIntegerWriteValue;
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
        // 目标类型编码) 全部下沉到 io 线程写路径完成 — 见 Gateway::WriteService。
        // 修复: 旧实现此处扫描运行时标签表判定"是否可写标签", 而该表由热重载
        //       在 io 线程整体替换, 跨线程读 vector = 数据竞争 (UB)。
        if (!jv->is_number()) {
            return Resp(400, "{\"error\":\"value 须为数值\"}");
        }
        value = jv->get<double>();
    } else {
        // bytes 路径 (P1 A): 接受 hex 字符串 ("01 0A 0B") 形态; 解析失败即 400
        if (!jb->is_string()) {
            return Resp(400, "{\"error\":\"bytes 须为字符串\"}");
        }
        const std::string b = jb->get<std::string>();
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

    MyProt::Gateway::WriteRequest wr;
    wr.tagName = jt->get<std::string>();
    wr.value = value;
    wr.bytes.swap(bytes);
    wr.readBack = readBack;
    const MyProt::Core::VoidExpected res =
        MyProt::Gateway::WriteService(*ctx.io, *ctx.gateway, *ctx.snapshot).Write(wr);
    if (!res.has_value()) {
        nlohmann::json err;
        // Error.context (变量名/设备名等定位信息) 一并回传 —
        //   丢失会让 "模板变量未提供" 这类错误无从定位.
        err["error"] = (res.error().message.empty()
                            ? "write failed" : res.error().message)
            + (res.error().context.empty()
                   ? std::string() : " [" + res.error().context + "]");
        // 503 触发条件 (P1 B + P1 C): read-back 不一致 或 写互斥 Busy
        // (与 write 502 区分: 503 = 服务端暂不可用, 客户端可重试)
        const int httpCode =
            (res.error().code == MyProt::Core::Error::Code::ReadBackMismatch
             || res.error().code == MyProt::Core::Error::Code::Busy)
            ? 503 : 502;
        return Resp(httpCode, err.dump());
    }
    return Resp(200, "{\"ok\":true}");
}

}} // namespace MyProt::App

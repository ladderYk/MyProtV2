// src/Southbound/src/PayloadMapper.cpp

#include "MyProt/Southbound/PayloadMapper.hpp"

#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace MyProt { namespace Southbound {

namespace {

const std::size_t kMaxPayloadBytes = 1024u * 1024u;

bool ParseDocument(const std::string& payload, nlohmann::json& doc, std::string& err) {
    if (payload.size() > kMaxPayloadBytes) {
        err = "payload 超过 1MB 上限";
        return false;
    }
    try {
        doc = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
        err = std::string("JSON 解析失败: ") + e.what();
        return false;
    }
    return true;
}

bool AsNumber(const nlohmann::json& j, double& d, std::string& err) {
    if (j.is_number()) {
        d = j.get<double>();
        return true;
    }
    if (j.is_boolean()) {
        d = j.get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        char* end = NULL;
        const double v = std::strtod(s.c_str(), &end);
        if (end == s.c_str() || *end != '\0') {
            err = "字符串无法转换为数值: \"" + s + "\"";
            return false;
        }
        d = v;
        return true;
    }
    err = "值不是数值";
    return false;
}

bool ByteArrayFromJson(const nlohmann::json& j,
                       std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) {
            if (!j[i].is_number_integer()) {
                err = "ByteArray 数组元素须为整数";
                return false;
            }
            const int64_t v = j[i].get<int64_t>();
            if (v < 0 || v > 255) {
                err = "ByteArray 元素超出 0-255";
                return false;
            }
            out.push_back(static_cast<uint8_t>(v));
        }
        return true;
    }
    if (j.is_string()) {
        static const char* kHex = "0123456789ABCDEF";
        const std::string s = j.get<std::string>();
        if (s.size() % 2 != 0) { err = "ByteArray hex 字符串长度须为偶数"; return false; }
        for (size_t i = 0; i + 1 < s.size(); i += 2) {
            int hi = -1, lo = -1;
            for (int k = 0; k < 16; ++k) {
                const char c = kHex[k];
                if (s[i] == c || s[i] == (c - 'A' + 'a')) hi = k;
                if (s[i + 1] == c || s[i + 1] == (c - 'A' + 'a')) lo = k;
            }
            if (hi < 0 || lo < 0) { err = "ByteArray hex 字符串含非法字符"; return false; }
            out.push_back(static_cast<uint8_t>(hi * 16 + lo));
        }
        return true;
    }
    err = "ByteArray 须为整数数组或 hex 字符串";
    return false;
}

} // namespace

bool PayloadMapper::MapTopicTag(const std::string& topic, int index1Based,
                                std::string& out) {
    out.clear();
    if (index1Based <= 0) return false;
    int index = 0;
    std::size_t begin = 0;
    while (begin <= topic.size()) {
        std::size_t end = topic.find('/', begin);
        if (end == std::string::npos) end = topic.size();
        ++index;
        if (index == index1Based) {
            out = topic.substr(begin, end - begin);
            return !out.empty();
        }
        if (end == topic.size()) break;
        begin = end + 1;
    }
    return false;
}

bool PayloadMapper::ToTypedValue(const nlohmann::json& j,
                                 const std::string& finalType,
                                 Core::TypedValue& out,
                                 std::string& err) {
    err.clear();
    out = Core::TypedValue();
    if (j.is_null()) { err = "值为 null"; return false; }

    if (finalType.empty() || finalType == "Auto") {
        if (j.is_boolean()) {
            out.type = Core::ValueType::Bool;
            out.b = j.get<bool>();
            return true;
        }
        if (j.is_number_unsigned()) {
            out.type = Core::ValueType::UInt64;
            out.u = j.get<uint64_t>();
            return true;
        }
        if (j.is_number_integer()) {
            out.type = Core::ValueType::Int64;
            out.i = j.get<int64_t>();
            return true;
        }
        if (j.is_number_float()) {
            out.type = Core::ValueType::Double;
            out.d = j.get<double>();
            return true;
        }
        if (j.is_string()) {
            out.type = Core::ValueType::String;
            out.str = j.get<std::string>();
            return true;
        }
        if (j.is_array()) {
            out.type = Core::ValueType::ByteArray;
            return ByteArrayFromJson(j, out.bytes, err);
        }
        err = "对象无法映射为单值";
        return false;
    }

    if (finalType == "String") {
        out.type = Core::ValueType::String;
        out.str = j.is_string() ? j.get<std::string>() : j.dump();
        return true;
    }
    if (finalType == "Bool") {
        out.type = Core::ValueType::Bool;
        if (j.is_boolean()) { out.b = j.get<bool>(); return true; }
        double d = 0.0;
        if (!AsNumber(j, d, err)) return false;
        out.b = (d != 0.0);
        return true;
    }
    if (finalType == "ByteArray") {
        out.type = Core::ValueType::ByteArray;
        return ByteArrayFromJson(j, out.bytes, err);
    }

    double d = 0.0;
    if (!AsNumber(j, d, err)) return false;
    if (!std::isfinite(d)) { err = "数值非有限"; return false; }

    if (finalType == "UInt16" || finalType == "UInt32" || finalType == "UInt64") {
        if (d < 0.0 || d > 18446744073709551615.0) { err = "数值超出无符号范围"; return false; }
        out.type = (finalType == "UInt16") ? Core::ValueType::UInt16
                 : (finalType == "UInt32") ? Core::ValueType::UInt32
                                           : Core::ValueType::UInt64;
        out.u = static_cast<uint64_t>(d);
        return true;
    }
    if (finalType == "Int16" || finalType == "Int32" || finalType == "Int64") {
        if (d < -9223372036854775808.0 || d > 9223372036854775807.0) {
            err = "数值超出有符号范围";
            return false;
        }
        out.type = (finalType == "Int16") ? Core::ValueType::Int16
                 : (finalType == "Int32") ? Core::ValueType::Int32
                                          : Core::ValueType::Int64;
        out.i = static_cast<int64_t>(d);
        return true;
    }
    if (finalType == "Float") {
        if (std::fabs(d) > FLT_MAX) { err = "数值超出 float 范围"; return false; }
        out.type = Core::ValueType::Float;
        out.d = static_cast<double>(static_cast<float>(d));
        return true;
    }
    if (finalType == "Double") {
        out.type = Core::ValueType::Double;
        out.d = d;
        return true;
    }

    err = "未知 finalType: " + finalType;
    return false;
}

bool PayloadMapper::MapJson(const std::string& payload,
                            const std::string& tagField,
                            const std::string& valueField,
                            const std::string& tsField,
                            const std::string& defaultTag,
                            std::vector<MappingItem>& out,
                            std::string& err) {
    out.clear();
    err.clear();

    nlohmann::json doc;
    if (!ParseDocument(payload, doc, err)) return false;

    if (doc.is_object()) {
        const std::string vf = valueField.empty() ? std::string("value") : valueField;
        if (doc.contains(vf)) {
            MappingItem item;
            const std::string tf = tagField.empty() ? std::string("tag") : tagField;
            if (doc.contains(tf) && doc[tf].is_string()) {
                item.tagName = doc[tf].get<std::string>();
            } else {
                item.tagName = defaultTag;
            }
            if (item.tagName.empty()) {
                err = "payload 缺少标签名 (tagField=" + tf + ", 无 topic 映射)";
                return false;
            }
            item.value = doc[vf];
            if (!tsField.empty() && doc.contains(tsField) && doc[tsField].is_number()) {
                const int64_t ts = doc[tsField].get<int64_t>();
                item.timestampMs = (ts > 0 && ts < 100000000000LL) ? ts * 1000 : ts;
            }
            out.push_back(item);
            return true;
        }

        // batch map: { "TagA": 1.5, "TagB": 2 }
        for (nlohmann::json::const_iterator it = doc.begin(); it != doc.end(); ++it) {
            if (!tsField.empty() && it.key() == tsField) continue;
            MappingItem item;
            item.tagName = it.key();
            item.value = it.value();
            if (!tsField.empty() && doc.contains(tsField) && doc[tsField].is_number()) {
                const int64_t ts = doc[tsField].get<int64_t>();
                item.timestampMs = (ts > 0 && ts < 100000000000LL) ? ts * 1000 : ts;
            }
            out.push_back(item);
        }
        if (out.empty()) { err = "payload 不含任何标签值"; return false; }
        return true;
    }

    if (defaultTag.empty()) {
        err = "payload 为标量/数组, 但未配置 topic 标签映射";
        return false;
    }
    MappingItem item;
    item.tagName = defaultTag;
    item.value = doc;
    out.push_back(item);
    return true;
}

}} // namespace MyProt::Southbound

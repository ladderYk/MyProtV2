// src/App/ValidateCli.cpp
// `--validate` 实现: 文件/目录两种入口, 复用加载期同一套校验器。
//
// 设计取舍:
//   - 不新写校验逻辑, 只做"取文本 → 调 ConfigValidator → 分类 → 打印";
//     因此 CLI 通过 == 网关能启动 (与 /api/validate 同源)。
//   - 目录模式逐文件校验而不是直接调 ConfigDirectoryLoader::Load: Load 是 fail-fast,
//     只会回第一条错误; CLI 的用处恰恰是"一次看全"。全部通过后再用 Load 复检一次,
//     补上 server.json / 仿真模板等只有加载器才覆盖的检查。
//   - 提示统一走 stdout (PowerShell 5.1 用 OEM 码页回读原生 stderr, UTF-8 会乱码)。

#include "ValidateCli.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "MyProt/Core/Config.hpp"
#include "MyProt/Core/Expected.hpp"
#include "MyProt/Core/Platform.hpp"
#include "MyProt/Service/ConfigDirectoryLoader.hpp"
#include "MyProt/Service/ConfigValidator.hpp"
#include "MyProt/Service/ValidationIssue.hpp"

namespace MyProt { namespace App {

namespace {

using MyProt::Service::ValidationIssue;
using MyProt::Service::ValidationResult;

struct FileReport {
    std::string path;
    std::string scope;                       // "protocol" | "tags" | "load"
    std::vector<ValidationIssue> issues;
};

bool ReadTextFile(const std::string& path, std::string& out) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size()
        && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ParentDir(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return p.substr(0, 1);
    return p.substr(0, slash);
}

/// 文件类型判定: 协议文件还是配置根 (tags.json)。
///   "protocolName" 是协议的唯一顶层必备标志; 但 JSON 本身语法错误时扫不到该键,
///   仍须把它当协议报语法错误 (否则会误拿去当配置根)。故补充父目录名判断。
bool LooksLikeProtocolFile(const std::string& path, const std::string& text) {
    if (text.find("\"protocolName\"") != std::string::npos) return true;
    const std::string dir = ParentDir(path);
    const std::size_t slash = dir.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? dir : dir.substr(slash + 1);
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] >= 'A' && name[i] <= 'Z') name[i] = static_cast<char>(name[i] - 'A' + 'a');
    }
    return name == "protocols" || name == "protocol";
}

/// 校验结果 -> 结构化 issue (warning 需补 "[WARN] " 前缀, 分类器据此定级)。
/// extraErrors: 深度校验之外额外补充的错误 (成帧试算)。
std::vector<ValidationIssue> Classify(const ValidationResult& r,
                                      const std::string& scope,
                                      const std::vector<std::string>& extraErrors) {
    std::vector<std::string> items;
    items.reserve(r.errors.size() + extraErrors.size() + r.warnings.size());
    items.insert(items.end(), r.errors.begin(), r.errors.end());
    items.insert(items.end(), extraErrors.begin(), extraErrors.end());
    for (std::size_t i = 0; i < r.warnings.size(); ++i) {
        items.push_back("[WARN] " + r.warnings[i]);
    }
    return MyProt::Service::ClassifyValidationItems(items, scope);
}

void AddParseIssue(FileReport& rep, const std::string& message) {
    ValidationIssue one;
    one.severity = "error";
    one.ruleId = "parse.error";
    one.scope = rep.scope;
    one.message = message;
    rep.issues.push_back(one);
}

/// 校验一份协议文本; 通过时把 POCO 文本留给根校验做跨文件引用。
bool ValidateProtocol(const std::string& path, const std::string& text,
                      FileReport& rep, std::string* outText) {
    rep.path = path;
    rep.scope = "protocol";
    const MyProt::Core::Expected<ValidationResult> vr =
        MyProt::Service::ConfigValidator::validateProtocolJson(
            text, MyProt::Core::kSupportedSchemaVersion);
    if (!vr.has_value()) {
        AddParseIssue(rep, vr.error().message);
        return false;
    }

    // 加载期的成帧试算只在"配置根 + 协议集"阶段执行, 单文件入口显式补上,
    // 否则 expr / 长度槽位不自洽要到网关启动才暴露, CLI 结论就不再等价于"能启动"。
    std::vector<std::string> frameErrors;
    if (!vr.value().hasErrors()) {
        const MyProt::Core::Expected<MyProt::Core::ProtocolConfig> poco =
            MyProt::Service::ConfigValidator::parseProtocolJson(text);
        if (poco.has_value()) {
            frameErrors = MyProt::Service::ConfigValidator::CheckFrameConsistency(poco.value());
        }
    }

    rep.issues = Classify(vr.value(), "protocol", frameErrors);
    if (vr.value().hasErrors() || !frameErrors.empty()) return false;
    if (outText) *outText = text;
    return true;
}

/// 校验 tags.json; protocolTexts 为已通过校验的协议文本 (跨文件引用)。
void ValidateRoot(const std::string& path, const std::string& text,
                  const std::vector<std::string>& protocolTexts,
                  FileReport& rep) {
    rep.path = path;
    rep.scope = "tags";
    const MyProt::Core::Expected<ValidationResult> vr =
        MyProt::Service::ConfigValidator::validateConfigRootJson(
            text, protocolTexts, MyProt::Core::kSupportedSchemaVersion);
    if (!vr.has_value()) {
        AddParseIssue(rep, vr.error().message);
        return;
    }
    rep.issues = Classify(vr.value(), "tags", std::vector<std::string>());
}

// ── 输出 ────────────────────────────────────────────────────

std::string JsonString(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('"');
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string IssueJson(const ValidationIssue& i) {
    std::string s = "{\"severity\":";
    s += JsonString(i.severity);
    s += ",\"ruleId\":";  s += JsonString(i.ruleId);
    s += ",\"subject\":"; s += JsonString(i.subject);
    s += ",\"field\":";   s += JsonString(i.field);
    s += ",\"pointer\":"; s += JsonString(i.pointer);
    s += ",\"message\":"; s += JsonString(i.message);
    s += "}";
    return s;
}

int PrintReports(const std::vector<FileReport>& reports, bool jsonOut) {
    int errors = 0;
    int warnings = 0;
    for (std::size_t i = 0; i < reports.size(); ++i) {
        for (std::size_t k = 0; k < reports[i].issues.size(); ++k) {
            if (reports[i].issues[k].severity == "error") ++errors;
            else ++warnings;
        }
    }

    if (jsonOut) {
        std::string body = "{\"ok\":";
        body += (errors == 0 ? "true" : "false");
        body += ",\"errorCount\":" + std::to_string(errors);
        body += ",\"warningCount\":" + std::to_string(warnings);
        body += ",\"files\":[";
        for (std::size_t i = 0; i < reports.size(); ++i) {
            if (i > 0) body += ",";
            body += "{\"path\":" + JsonString(reports[i].path);
            body += ",\"scope\":" + JsonString(reports[i].scope);
            body += ",\"issues\":[";
            for (std::size_t k = 0; k < reports[i].issues.size(); ++k) {
                if (k > 0) body += ",";
                body += IssueJson(reports[i].issues[k]);
            }
            body += "]}";
        }
        body += "]}";
        std::cout << body << std::endl;
        return errors == 0 ? 0 : 1;
    }

    for (std::size_t i = 0; i < reports.size(); ++i) {
        std::cout << reports[i].path << std::endl;
        if (reports[i].issues.empty()) {
            std::cout << "  [ OK  ] 无问题" << std::endl;
            continue;
        }
        for (std::size_t k = 0; k < reports[i].issues.size(); ++k) {
            const ValidationIssue& issue = reports[i].issues[k];
            std::cout << "  " << (issue.severity == "error" ? "[ERROR]" : "[WARN ]")
                      << " " << (issue.pointer.empty() ? "(file)" : issue.pointer)
                      << "  " << issue.message << std::endl;
        }
    }
    std::cout << std::endl
              << "共 " << reports.size() << " 项: "
              << errors << " 个错误, " << warnings << " 个警告" << std::endl;
    return errors == 0 ? 0 : 1;
}

// ── 入口 ────────────────────────────────────────────────────

int ValidateDirectory(const std::string& dir, bool jsonOut) {
    std::vector<FileReport> reports;
    std::vector<std::string> protocolTexts;

    const std::string protoDir = dir + MyProt::Service::ConfigLayout::kProtocolsDirPath;
    const std::vector<std::string> protoFiles =
        MyProt::Core::Platform::ListFiles(protoDir, ".json");
    if (protoFiles.empty()) {
        FileReport rep;
        rep.path = protoDir;
        rep.scope = "protocol";
        AddParseIssue(rep, "目录下没有 *.json 协议文件 (或目录不存在)");
        reports.push_back(rep);
    }
    for (std::size_t i = 0; i < protoFiles.size(); ++i) {
        const std::string path = protoDir + "/" + protoFiles[i];
        FileReport rep;
        std::string text;
        if (!ReadTextFile(path, text)) {
            rep.path = path;
            rep.scope = "protocol";
            AddParseIssue(rep, "无法读取文件");
        } else {
            std::string validated;
            ValidateProtocol(path, text, rep, &validated);
            // 仅收集通过校验的协议文本 (跨文件引用校验的输入)
            if (!validated.empty()) protocolTexts.push_back(validated);
        }
        reports.push_back(rep);
    }

    const std::string rootPath = dir + MyProt::Service::ConfigLayout::kTagsFilePath;
    {
        FileReport rep;
        std::string text;
        if (!ReadTextFile(rootPath, text)) {
            rep.path = rootPath;
            rep.scope = "tags";
            AddParseIssue(rep, "无法读取文件 (配置根必需)");
        } else {
            ValidateRoot(rootPath, text, protocolTexts, rep);
        }
        reports.push_back(rep);
    }

    int errors = 0;
    for (std::size_t i = 0; i < reports.size(); ++i) {
        for (std::size_t k = 0; k < reports[i].issues.size(); ++k) {
            if (reports[i].issues[k].severity == "error") ++errors;
        }
    }
    // 逐文件全通过后, 再用真实加载器复检 (覆盖 server.json / 仿真模板 / 别名归一化等)
    if (errors == 0) {
        const MyProt::Core::Expected<MyProt::Service::LoadedConfig> loaded =
            MyProt::Service::ConfigDirectoryLoader::Load(
                dir, MyProt::Core::kSupportedSchemaVersion);
        if (!loaded.has_value()) {
            FileReport rep;
            rep.path = dir + MyProt::Service::ConfigLayout::kServerFilePath;
            rep.scope = "load";
            AddParseIssue(rep, loaded.error().message
                              + (loaded.error().context.empty()
                                     ? std::string()
                                     : (": " + loaded.error().context)));
            reports.push_back(rep);
        }
    }

    return PrintReports(reports, jsonOut);
}

} // namespace

int RunValidate(const std::string& target, bool jsonOut) {
    MyProt::Core::Platform::EnableUtf8Console();

    if (target.empty()) {
        std::cout << "用法: MyProt.App.exe --validate <file.json|dir> [--json]" << std::endl;
        return 2;
    }
    if (!MyProt::Core::Platform::FileExists(target)) {
        std::cout << "--validate 目标不存在: " << target << std::endl;
        return 2;
    }

    if (!EndsWith(target, ".json")) {
        const int rc = ValidateDirectory(target, jsonOut);
        return rc;
    }

    std::vector<FileReport> reports;
    std::string text;
    if (!ReadTextFile(target, text)) {
        std::cout << "无法读取: " << target << std::endl;
        return 2;
    }

    // 文件类型判定: 协议文件 → 单文件校验; 否则按配置根处理。
    const bool isProtocol = LooksLikeProtocolFile(target, text);
    FileReport rep;
    if (isProtocol) {
        ValidateProtocol(target, text, rep, 0);
    } else {
        const std::string dir = ParentDir(target);
        std::vector<std::string> protocolTexts;
        const std::string protoDir = dir + MyProt::Service::ConfigLayout::kProtocolsDirPath;
        const std::vector<std::string> protoFiles =
            MyProt::Core::Platform::ListFiles(protoDir, ".json");
        for (std::size_t i = 0; i < protoFiles.size(); ++i) {
            std::string protoText;
            if (ReadTextFile(protoDir + "/" + protoFiles[i], protoText)) {
                protocolTexts.push_back(protoText);
            }
        }
        ValidateRoot(target, text, protocolTexts, rep);
    }
    reports.push_back(rep);
    return PrintReports(reports, jsonOut);
}

}} // namespace MyProt::App

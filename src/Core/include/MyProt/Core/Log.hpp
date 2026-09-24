// src/Core/include/MyProt/Core/Log.hpp
// Structured logging facade - level filtering + timestamp + thread-safe console/file dual sink
// (architecture/05 observability conventions; header-only, consistent with the Core project convention)
// Usage: LOG_INFO("Polling", "device %s poll complete", dev.c_str());
//       Set the level via the environment variable MYPROT_LOG_LEVEL=debug|info|warn|error,
//       MYPROT_LOG_FILE=<path> enables file output with rotation (10MB x 3 copies, wired by the host program).
#pragma once
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <mutex>
#include <fstream>
#include <chrono>
#include <ctime>
#include <thread>
#include <functional>

namespace MyProt { namespace Core {

/// Log level - the numeric ordering is used for threshold comparison
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

class Logger {
public:
    /// Set the global minimum output level (logs below it are dropped); defaults to Info
    static void SetLevel(LogLevel lv) { S().level = lv; }
    static LogLevel GetLevel() { return S().level; }

    /// Enable file output (append; over rotateBytes it rotates to <path>.1 and shifts older copies,
    /// keeping at most keepBackups history files)
    static void SetFileSink(const std::string& path,
                            std::size_t rotateBytes = 10u * 1024u * 1024u,
                            int keepBackups = 3);

    /// level switch - for macro short-circuiting, avoiding the formatting cost below the threshold
    static bool Enabled(LogLevel lv) {
        return static_cast<int>(lv) >= static_cast<int>(S().level);
    }

    /// printf-style write of one log line; tag is the module name ("App"/"Polling"/"Gateway"/...)
    static void Write(LogLevel lv, const char* tag, const char* fmt, ...);

private:
    struct State {
        State() : level(LogLevel::Info), rotateBytes(0), keepBackups(3),
                  written(0) {}
        std::mutex mtx;
        LogLevel level;
        std::ofstream file;
        std::string filePath;
        std::size_t rotateBytes;
        int keepBackups;
        std::size_t written;
    };
    static State& S() { static State st; return st; }

    static const char* LevelName(LogLevel lv) {
        switch (lv) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO";
            case LogLevel::Warn:  return "WARN";
            default:              return "ERROR";
        }
    }

    /// On file overflow, shift backups and reopen (must be called with the lock held)
    static void RotateLocked(State& st);
};

inline void Logger::SetFileSink(const std::string& path,
                                std::size_t rotateBytes, int keepBackups) {
    State& st = S();
    std::lock_guard<std::mutex> lock(st.mtx);
    if (st.file.is_open()) st.file.close();
    st.filePath = path;
    st.rotateBytes = rotateBytes;
    st.keepBackups = keepBackups > 0 ? keepBackups : 1;
    st.file.open(path.c_str(), std::ios::app);
    st.written = st.file.is_open()
        ? static_cast<std::size_t>(st.file.tellp()) : 0;
}

inline void Logger::RotateLocked(State& st) {
    st.file.close();
    for (int i = st.keepBackups - 1; i >= 1; --i) {
        std::string from = st.filePath + "." + std::to_string(i);
        std::string to   = st.filePath + "." + std::to_string(i + 1);
        std::remove(to.c_str());   // clear the target slot first, then shift (reversing this would delete the source and rotation would never take effect)
        std::rename(from.c_str(), to.c_str());
    }
    std::remove((st.filePath + ".1").c_str());
    std::rename(st.filePath.c_str(), (st.filePath + ".1").c_str());
    st.file.open(st.filePath.c_str(), std::ios::app | std::ios::trunc);
    st.written = 0;
}

inline void Logger::Write(LogLevel lv, const char* tag, const char* fmt, ...) {
    State& st = S();

    // format the message body (stack buffer, truncation-safe)
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (n < 0) { msg[0] = '\0'; n = 0; }
    else if (static_cast<size_t>(n) >= sizeof(msg)) n = sizeof(msg) - 1;

    // timestamp: local time + milliseconds
    using std::chrono::system_clock;
    system_clock::time_point now = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
    int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif

    unsigned tid = static_cast<unsigned>(
        std::hash<std::thread::id>()(std::this_thread::get_id())) % 0x10000u;

    char head[96];
    std::snprintf(head, sizeof(head),
                  "[%04d-%02d-%02d %02d:%02d:%02d.%03d][%s][%s][%04x] ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms,
                  LevelName(lv), tag ? tag : "?", tid);

    std::lock_guard<std::mutex> lock(st.mtx);
    FILE* console = (lv == LogLevel::Warn || lv == LogLevel::Error)
                        ? stderr : stdout;
    std::fwrite(head, 1, std::strlen(head), console);
    std::fwrite(msg, 1, static_cast<size_t>(n), console);
    std::fputc('\n', console);
    std::fflush(console);

    if (st.file.is_open()) {
        st.file.write(head, std::strlen(head));
        st.file.write(msg, n);
        st.file.put('\n');
        st.file.flush();
        st.written += std::strlen(head) + static_cast<size_t>(n) + 1;
        if (st.rotateBytes > 0 && st.written >= st.rotateBytes) {
            RotateLocked(st);
        }
    }
}

}} // namespace MyProt::Core

/// convenience macros - tag and fmt are required; the MSVC traditional preprocessor tolerates empty varargs via a trailing-comma omission
#define MYPROT_LOG(lv, tag, ...)                                              \
    do {                                                                      \
        if (MyProt::Core::Logger::Enabled(lv)) {                              \
            MyProt::Core::Logger::Write((lv), (tag), __VA_ARGS__);            \
        }                                                                     \
    } while (0)
#define LOG_DEBUG(tag, ...) \
    MYPROT_LOG(MyProt::Core::LogLevel::Debug, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...) \
    MYPROT_LOG(MyProt::Core::LogLevel::Info, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...) \
    MYPROT_LOG(MyProt::Core::LogLevel::Warn, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) \
    MYPROT_LOG(MyProt::Core::LogLevel::Error, tag, __VA_ARGS__)

// src/Core/include/MyProt/Core/Platform.hpp
// Cross-platform OS helpers - the single implementation point for environment,
// console, and file-system calls that differ between Windows and POSIX.
// Header-only, C++11, no third-party dependency.
//
// Design notes:
//   - Public headers must stay free of <windows.h>; this header is the only
//     place where Win32 file APIs are allowed.
//   - POSIX paths are treated as UTF-8 byte strings; Windows paths are widened
//     to UTF-16 so non-ASCII configuration paths keep working.

#pragma once

#include <algorithm>
#include <cerrno>
#include <string>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <cstdlib>
#  include <cstdio>
#  include <dirent.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace MyProt { namespace Core { namespace Platform {

#if defined(_WIN32)
/// UTF-8 -> UTF-16 (Win32 wide-character APIs). Empty result on invalid input.
inline std::wstring ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n) - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
#endif

/// Read an environment variable. Returns an empty string when unset.
inline std::string GetEnv(const char* name) {
    if (name == nullptr) return std::string();
#if defined(_WIN32)
    size_t required = 0;
    if (getenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
        return std::string();
    }
    std::vector<char> buf(required);
    size_t written = 0;
    if (getenv_s(&written, &buf[0], required, name) != 0) {
        return std::string();
    }
    return std::string(&buf[0], written > 0 ? written - 1 : 0);  // drop trailing '\0'
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

/// Switch the console to UTF-8. No-op on POSIX (terminals are already UTF-8).
inline void EnableUtf8Console() {
#if defined(_WIN32)
    ::SetConsoleOutputCP(65001);
#endif
}

/// True when the path exists (regular file or directory).
inline bool FileExists(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::wstring w = ToWide(path);
    if (w.empty()) return false;
    return ::GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
#endif
}

/// Create a single directory level. Returns true when it already exists or was
/// created; the parent directory must exist beforehand.
inline bool EnsureDirectory(const std::string& path) {
    if (path.empty()) return false;
#if defined(_WIN32)
    const std::wstring w = ToWide(path);
    if (w.empty()) return false;
    if (::CreateDirectoryW(w.c_str(), NULL) != 0) return true;
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (::mkdir(path.c_str(), 0755) == 0) return true;
    return errno == EEXIST;
#endif
}

/// Atomically replace |to| with |from| (rename semantics).
inline bool AtomicReplaceFile(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    const std::wstring wFrom = ToWide(from);
    const std::wstring wTo = ToWide(to);
    if (wFrom.empty() || wTo.empty()) return false;
    return ::MoveFileExW(wFrom.c_str(), wTo.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

/// Remove a file; returns true when it no longer exists.
inline bool RemoveFile(const std::string& path) {
#if defined(_WIN32)
    const std::wstring w = ToWide(path);
    if (w.empty()) return false;
    return ::DeleteFileW(w.c_str()) != 0;
#else
    return ::unlink(path.c_str()) == 0;
#endif
}

/// List regular file names (without directory prefix) whose name ends with
/// |suffix|; the result is sorted so configuration load order stays stable.
inline std::vector<std::string> ListFiles(const std::string& dir,
                                          const std::string& suffix) {
    std::vector<std::string> names;
#if defined(_WIN32)
    std::string base = dir;
    if (!base.empty() && (base[base.size() - 1] != '/' && base[base.size() - 1] != '\\')) {
        base += '/';
    }
    const std::wstring pattern = ToWide(base + "*" + suffix);
    if (pattern.empty()) return names;

    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 1) continue;
        std::vector<char> utf8(static_cast<size_t>(need));
        ::WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, &utf8[0], need,
                              nullptr, nullptr);
        names.push_back(std::string(&utf8[0]));
    } while (::FindNextFileW(h, &fd) != 0);
    ::FindClose(h);
#else
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return names;
    const size_t suffixLen = suffix.size();
    while (struct dirent* entry = ::readdir(d)) {
        const std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (suffixLen > 0
                && (name.size() < suffixLen
                    || name.compare(name.size() - suffixLen, suffixLen, suffix) != 0)) {
            continue;
        }
        struct stat st;
        const std::string full = dir + "/" + name;
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        names.push_back(name);
    }
    ::closedir(d);
#endif
    std::sort(names.begin(), names.end());
    return names;
}

}}} // namespace MyProt::Core::Platform

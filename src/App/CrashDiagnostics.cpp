// src/App/CrashDiagnostics.cpp — 崩溃取证 (方案1-S1 从 RuntimeGlue.cpp 搬移, 零行为变更)

#include "CrashDiagnostics.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>

#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>

namespace MyProt { namespace App {
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

}} // namespace MyProt::App

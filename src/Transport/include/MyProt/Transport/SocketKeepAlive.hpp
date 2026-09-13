// src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp
// 跨平台 TCP keepalive 设置 — 解决"对端关闭后无法感知"问题 (2026-08-29 实装)
// 机制: 让 OS 内核周期性探测死连接, 对端 RST 后 socket read 立即返回 error,
//       已有 SendReceive 错误路径自然走通, 下次轮询自动重连。
// 跨平台: Windows 用 SIO_KEEPALIVE_VALS (ms 级), Linux/BSD 用 TCP_KEEP* setsockopt (s 级)。

#pragma once

#include "MyProt/Transport/NativeSocket.hpp"   // NativeSocket 类型定义

#if defined(_WIN32)
    #include <winsock2.h>
    #include <mstcpip.h>     // SIO_KEEPALIVE_VALS / tcp_keepalive
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

namespace MyProt { namespace Transport {

/// 启用/配置 TCP keepalive
/// @param s        原生 socket 句柄 (asio::ip::tcp::socket::native_handle())
/// @param onoff    true 启用, false 关闭
/// @param idleSec  空闲多久后开始探测 (秒; Linux 下有效; Windows 用 idleMs)
/// @param intvlSec 探测间隔 (秒; Linux 下有效; Windows 用 intvlMs)
/// @param cnt      探测失败次数 (Linux 下有效; Windows 忽略, 固定 3 探测 ≈ 3×intvl)
inline void SetSocketKeepAlive(NativeSocket s, bool onoff,
                               int idleSec, int intvlSec, int cnt) {
    if (s == InvalidSocket()) return;
#if defined(_WIN32)
    (void)cnt;   // Windows 的 SIO_KEEPALIVE_VALS 无探测次数参数 (固定 3 次), 仅 POSIX 分支使用
    struct {
        ULONG onoff;
        ULONG keepalivetime;      // ms
        ULONG keepaliveinterval;  // ms
    } in = {
        onoff ? 1UL : 0UL,
        static_cast<ULONG>(idleSec) * 1000UL,
        static_cast<ULONG>(intvlSec) * 1000UL
    };
    DWORD bytesReturned = 0;
    ::WSAIoctl(s, SIO_KEEPALIVE_VALS, &in, sizeof(in),
               nullptr, 0, &bytesReturned, nullptr, nullptr);
    // 注: SIO_KEEPALIVE_VALS 失败通常是因为对端非 TCP (如串口);
    //     调用方需自行判断 socket 类型, 此处静默返回。
#else
    int opt = onoff ? 1 : 0;
    ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE,  &idleSec,  sizeof(idleSec));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvlSec, sizeof(intvlSec));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,      sizeof(cnt));
#endif
}

}} // namespace MyProt::Transport

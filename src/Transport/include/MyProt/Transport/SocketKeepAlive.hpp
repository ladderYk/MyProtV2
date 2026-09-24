// src/Transport/include/MyProt/Transport/SocketKeepAlive.hpp
// Cross-platform TCP keepalive setup - solves the "cannot detect a peer-close" problem (implemented 2026-08-29)
// Mechanism: let the OS kernel periodically probe dead connections; after the peer sends RST, socket read returns an error immediately,
//       the existing SendReceive error path is naturally exercised, and the next poll reconnects automatically.
// Cross-platform: Windows uses SIO_KEEPALIVE_VALS (ms granularity), Linux/BSD uses TCP_KEEP* setsockopt (s granularity).

#pragma once

#include "MyProt/Transport/NativeSocket.hpp"   // NativeSocket type definition

#if defined(_WIN32)
    #include <winsock2.h>
    #include <mstcpip.h>     // SIO_KEEPALIVE_VALS / tcp_keepalive
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

namespace MyProt { namespace Transport {

/// Enable/configure TCP keepalive
/// @param s        native socket handle (asio::ip::tcp::socket::native_handle())
/// @param onoff    true to enable, false to disable
/// @param idleSec  how long idle before probing starts (seconds; effective on Linux; Windows uses idleMs)
/// @param intvlSec probe interval (seconds; effective on Linux; Windows uses intvlMs)
/// @param cnt      number of failed probes (effective on Linux; ignored on Windows, fixed at 3 probes ≈ 3×intvl)
inline void SetSocketKeepAlive(NativeSocket s, bool onoff,
                               int idleSec, int intvlSec, int cnt) {
    if (s == InvalidSocket()) return;
#if defined(_WIN32)
    (void)cnt;   // Windows' SIO_KEEPALIVE_VALS has no probe-count parameter (fixed at 3); only the POSIX branch uses it
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
    // Note: SIO_KEEPALIVE_VALS usually fails because the peer is not TCP (e.g. a serial port);
    //       the caller must determine the socket type itself; here it returns silently.
#else
    int opt = onoff ? 1 : 0;
    ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE,  &idleSec,  sizeof(idleSec));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvlSec, sizeof(intvlSec));
    ::setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT,   &cnt,      sizeof(cnt));
#endif
}

}} // namespace MyProt::Transport

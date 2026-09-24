// src/Transport/include/MyProt/Transport/NativeSocket.hpp
// Cross-platform abstraction for the native socket handle (implemented 2026-08-29, pairs with SocketKeepAlive.hpp)
// Windows: SOCKET (UINT_PTR) | Linux/BSD: int

#pragma once

#if defined(_WIN32)
    #include <winsock2.h>
    namespace MyProt { namespace Transport {
        typedef ::SOCKET NativeSocket;
        inline NativeSocket InvalidSocket() { return INVALID_SOCKET; }
    }}
#else
    namespace MyProt { namespace Transport {
        typedef int NativeSocket;
        inline NativeSocket InvalidSocket() { return -1; }
    }}
#endif

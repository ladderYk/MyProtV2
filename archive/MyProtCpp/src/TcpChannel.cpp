#include "TcpChannel.h"
#include <iostream>
#include <ws2tcpip.h>
#include <mutex>

namespace MyProt
{
    TcpChannel::TcpChannel(const FramingConfig& framing)
        : _framing(framing)
    {
        static std::once_flag wsaInitFlag;
        std::call_once(wsaInitFlag, []() {
            WSADATA wsaData;
            WSAStartup(MAKEWORD(2, 2), &wsaData);
        });
    }

    TcpChannel::~TcpChannel()
    {
        Disconnect();
    }

    bool TcpChannel::Connect(const std::string& host, int port, int timeoutMs)
    {
        Disconnect();

        _host = host;
        _port = port;
        _timeoutMs = timeoutMs;

        _socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (_socket == INVALID_SOCKET)
        {
            std::cerr << "Socket creation failed: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(static_cast<u_short>(port));

        in_addr addr{};
        if (inet_pton(AF_INET, host.c_str(), &addr) != 1)
        {
            std::cerr << "Invalid IP address: " << host << std::endl;
            SafeClose(_socket);
            return false;
        }
        serverAddr.sin_addr = addr;

        unsigned long nonBlocking = 1;
        if (ioctlsocket(_socket, FIONBIO, &nonBlocking) != 0)
        {
            std::cerr << "Failed to set non-blocking: " << WSAGetLastError() << std::endl;
            SafeClose(_socket);
            return false;
        }

        int result = connect(_socket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (result == SOCKET_ERROR)
        {
            int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
            {
                std::cerr << "Connect failed: " << error << std::endl;
                SafeClose(_socket);
                return false;
            }
        }

        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(_socket, &writeSet);

        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        result = select(0, nullptr, &writeSet, nullptr, &timeout);
        if (result <= 0)
        {
            std::cerr << "Connect timeout" << std::endl;
            SafeClose(_socket);
            return false;
        }

        int optval;
        socklen_t optlen = sizeof(optval);
        if (getsockopt(_socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&optval), &optlen) != 0 || optval != 0)
        {
            std::cerr << "Connect failed: " << optval << std::endl;
            SafeClose(_socket);
            return false;
        }

        nonBlocking = 0;
        ioctlsocket(_socket, FIONBIO, &nonBlocking);

        // 设置接收超时（Windows SO_RCVTIMEO 接受 DWORD 毫秒值）
        DWORD recvTimeout = static_cast<DWORD>(timeoutMs);
        if (setsockopt(_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout)) != 0)
        {
            std::cerr << "Failed to set recv timeout: " << WSAGetLastError() << std::endl;
        }

        _isConnected.store(true);
        std::cout << "Connected to " << host << ":" << port << std::endl;
        return true;
    }

    void TcpChannel::Disconnect()
    {
        std::lock_guard<std::mutex> lock(_disconnectMutex);
        _isConnected.store(false);
        SafeClose(_socket);
        _socket = INVALID_SOCKET;
    }

    bool TcpChannel::SendReceive(const std::vector<uint8_t>& request, std::vector<uint8_t>& response,
        const FramingConfig* overrideFraming)
    {
        std::lock_guard<std::mutex> lock(_sendMutex);

        if (!_isConnected.load())
        {
            std::cerr << "Not connected" << std::endl;
            return false;
        }

        int bytesSent = send(_socket, reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size()), 0);
        if (bytesSent != static_cast<int>(request.size()))
        {
            std::cerr << "Send failed: " << WSAGetLastError() << std::endl;
            _isConnected.store(false);
            return false;
        }

        const FramingConfig& framing = overrideFraming ? *overrideFraming : _framing;

        if (framing.type == "Fixed")
        {
            if (!framing.fixedLength.has_value())
            {
                std::cerr << "Fixed length not specified" << std::endl;
                return false;
            }
            return ReadFixed(framing.fixedLength.value(), response);
        }
        else if (framing.type == "LengthField")
        {
            return ReadLengthFieldFrame(framing, response);
        }
        else
        {
            std::cerr << "Unsupported framing type: " << framing.type << std::endl;
            return false;
        }
    }

    void TcpChannel::SafeClose(SOCKET socket)
    {
        if (socket != INVALID_SOCKET)
        {
            try
            {
                shutdown(socket, SD_BOTH);
            }
            catch (...) {}

            try
            {
                closesocket(socket);
            }
            catch (...) {}
        }
    }

    bool TcpChannel::ReadFixed(int length, std::vector<uint8_t>& buffer)
    {
        buffer.resize(length);
        return ReadExact(buffer, 0, length);
    }

    bool TcpChannel::ReadLengthFieldFrame(const FramingConfig& f, std::vector<uint8_t>& buffer)
    {
        if (!f.lengthFieldOffset.has_value() || !f.lengthFieldLength.has_value())
        {
            std::cerr << "Length field config incomplete" << std::endl;
            return false;
        }

        int minHeader = f.lengthFieldOffset.value() + f.lengthFieldLength.value();
        std::vector<uint8_t> header(minHeader);
        
        if (!ReadExact(header, 0, minHeader))
        {
            return false;
        }

        long lengthValue = 0;
        for (int i = 0; i < f.lengthFieldLength.value(); ++i)
        {
            int pos = f.lengthFieldOffset.value() + i;
            if (f.byteOrder == "LittleEndian")
            {
                lengthValue |= static_cast<long>(header[pos]) << (8 * i);
            }
            else
            {
                lengthValue = (lengthValue << 8) | header[pos];
            }
        }

        int totalLength;
        if (f.lengthIncludesHeader.has_value() && f.lengthIncludesHeader.value())
        {
            totalLength = static_cast<int>(lengthValue);
        }
        else
        {
            int headerLen = f.headerLength.has_value() ? f.headerLength.value() : 0;
            totalLength = headerLen + static_cast<int>(lengthValue);
        }

        if (totalLength > minHeader)
        {
            buffer.resize(totalLength);
            std::copy(header.begin(), header.end(), buffer.begin());
            return ReadExact(buffer, minHeader, totalLength - minHeader);
        }
        else
        {
            buffer = std::move(header);
            return true;
        }
    }

    bool TcpChannel::ReadExact(std::vector<uint8_t>& buffer, int offset, int count)
    {
        while (count > 0)
        {
            int bytesRead = recv(_socket, reinterpret_cast<char*>(buffer.data() + offset), count, 0);
            if (bytesRead <= 0)
            {
                std::cerr << "Read failed: " << WSAGetLastError() << std::endl;
                _isConnected.store(false);
                return false;
            }
            offset += bytesRead;
            count -= bytesRead;
        }
        return true;
    }
}

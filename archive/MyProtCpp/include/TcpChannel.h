#pragma once

#include "ProtocolConfig.h"
#include <vector>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>
#include <winsock2.h>

namespace MyProt
{
    class TcpChannel : public std::enable_shared_from_this<TcpChannel>
    {
    public:
        TcpChannel(const FramingConfig& framing);
        ~TcpChannel();

        bool IsConnected() const { return _isConnected.load(); }

        /// <summary>
        /// 连接到设备
        /// </summary>
        bool Connect(const std::string& host, int port, int timeoutMs = 1000);

        /// <summary>
        /// 断开连接
        /// </summary>
        void Disconnect();

        /// <summary>
        /// 发送请求并接收响应（带并发控制）
        /// </summary>
        bool SendReceive(const std::vector<uint8_t>& request, std::vector<uint8_t>& response,
            const FramingConfig* overrideFraming = nullptr);

    private:
        SOCKET _socket = INVALID_SOCKET;
        std::string _host;
        int _port = 0;
        int _timeoutMs = 1000;
        FramingConfig _framing;
        std::atomic<bool> _isConnected{ false };
        
        // 并发控制
        std::mutex _sendMutex;
        std::mutex _disconnectMutex;

        /// <summary>
        /// 安全关闭Socket
        /// </summary>
        void SafeClose(SOCKET socket);

        /// <summary>
        /// 读取固定长度数据
        /// </summary>
        bool ReadFixed(int length, std::vector<uint8_t>& buffer);

        /// <summary>
        /// 按长度字段模式读取帧
        /// </summary>
        bool ReadLengthFieldFrame(const FramingConfig& f, std::vector<uint8_t>& buffer);

        /// <summary>
        /// 精确读取指定字节数
        /// </summary>
        bool ReadExact(std::vector<uint8_t>& buffer, int offset, int count);
    };
}

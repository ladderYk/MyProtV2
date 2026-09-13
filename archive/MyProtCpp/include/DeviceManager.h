#pragma once

#include "ProtocolConfig.h"
#include "TcpChannel.h"
#include "ProtocolEngine.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>

namespace MyProt
{
    class DeviceManager
    {
    public:
        DeviceManager(const std::vector<DeviceConfig>& devices, 
                      const std::map<std::string, ProtocolConfig>& protocols);
        ~DeviceManager();

        /// <summary>
        /// 启动时调用：为所有设备建立连接
        /// </summary>
        void ConnectAll();

        /// <summary>
        /// 获取设备通道（内部含重连逻辑）
        /// </summary>
        std::shared_ptr<TcpChannel> GetChannel(const std::string& deviceId);

        /// <summary>
        /// 获取设备配置
        /// </summary>
        const DeviceConfig* GetDevice(const std::string& deviceId) const;

        /// <summary>
        /// 获取协议配置
        /// </summary>
        const ProtocolConfig* GetProtocol(const std::string& protocolName) const;

        /// <summary>
        /// 手动触发设备重连
        /// </summary>
        bool TriggerReconnect(const std::string& deviceId);

        /// <summary>
        /// 停止所有重连线程
        /// </summary>
        void StopAll();

        /// <summary>
        /// 获取所有设备状态
        /// </summary>
        std::map<std::string, bool> GetDeviceStatuses() const;

        /// <summary>
        /// 检查单个设备是否在线
        /// </summary>
        bool IsDeviceConnected(const std::string& deviceId) const;

    private:
        std::map<std::string, DeviceConfig> _deviceConfigs;
        std::map<std::string, ProtocolConfig> _protocolConfigs;
        std::map<std::string, std::shared_ptr<TcpChannel>> _channels;
        
        // 重连相关 - 使用 unique_ptr 包装 atomic 避免拷贝问题
        std::map<std::string, std::thread> _reconnectThreads;
        std::map<std::string, std::unique_ptr<std::atomic<bool>>> _reconnectFlags;
        std::map<std::string, std::unique_ptr<std::atomic<int>>> _reconnectAttempts;
        
        mutable std::mutex _channelsMutex;
        std::atomic<bool> _stopping{ false };
        std::shared_ptr<std::atomic<bool>> _stoppingFlag;

        /// <summary>
        /// 连接单个设备
        /// </summary>
        bool ConnectDevice(const DeviceConfig& device);

        /// <summary>
        /// 执行握手流程
        /// </summary>
        bool ExecuteHandshake(std::shared_ptr<TcpChannel> channel, const ProtocolConfig& proto);

        /// <summary>
        /// 重连线程函数
        /// </summary>
        void ReconnectLoop(const std::string& deviceId);

        /// <summary>
        /// 验证响应条件
        /// </summary>
        static bool Validate(const std::string& condition, const std::vector<uint8_t>& resp);

        /// <summary>
        /// 创建通道
        /// </summary>
        std::shared_ptr<TcpChannel> CreateChannel(const FramingConfig& framing);

        /// <summary>
        /// 内部重连触发（假设已持有锁）
        /// </summary>
        bool TriggerReconnectLocked(const std::string& deviceId);
    };
}

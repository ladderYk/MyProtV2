#include "DeviceManager.h"
#include <iostream>
#include <chrono>
#include <regex>

namespace MyProt
{
    DeviceManager::DeviceManager(const std::vector<DeviceConfig>& devices,
        const std::map<std::string, ProtocolConfig>& protocols)
    {
        for (const auto& dev : devices)
        {
            _deviceConfigs[dev.id] = dev;
            _reconnectFlags[dev.id] = std::unique_ptr<std::atomic<bool>>(new std::atomic<bool>(false));
            _reconnectAttempts[dev.id] = std::unique_ptr<std::atomic<int>>(new std::atomic<int>(0));
        }
        _protocolConfigs = protocols;
        _stoppingFlag = std::make_shared<std::atomic<bool>>(false);
    }

    DeviceManager::~DeviceManager()
    {
        StopAll();
    }

    void DeviceManager::ConnectAll()
    {
        _stopping.store(false);
        *_stoppingFlag = false;

        for (const auto& pair : _deviceConfigs)
        {
            const DeviceConfig& device = pair.second;
            if (!ConnectDevice(device))
            {
                std::cout << "Device " << device.id << " initial connection failed, starting reconnect thread" << std::endl;
                TriggerReconnect(device.id);
            }
        }
    }

    std::shared_ptr<TcpChannel> DeviceManager::GetChannel(const std::string& deviceId)
    {
        std::lock_guard<std::mutex> lock(_channelsMutex);

        auto it = _channels.find(deviceId);
        if (it != _channels.end())
        {
            if (it->second->IsConnected())
            {
                return it->second;
            }
            else
            {
                TriggerReconnectLocked(deviceId);
            }
        }

        return nullptr;
    }

    const DeviceConfig* DeviceManager::GetDevice(const std::string& deviceId) const
    {
        auto it = _deviceConfigs.find(deviceId);
        return it != _deviceConfigs.end() ? &it->second : nullptr;
    }

    const ProtocolConfig* DeviceManager::GetProtocol(const std::string& protocolName) const
    {
        auto it = _protocolConfigs.find(protocolName);
        return it != _protocolConfigs.end() ? &it->second : nullptr;
    }

    bool DeviceManager::TriggerReconnect(const std::string& deviceId)
    {
        std::lock_guard<std::mutex> lock(_channelsMutex);
        return TriggerReconnectLocked(deviceId);
    }

    bool DeviceManager::TriggerReconnectLocked(const std::string& deviceId)
    {
        auto it = _deviceConfigs.find(deviceId);
        if (it == _deviceConfigs.end())
        {
            std::cerr << "Device " << deviceId << " not found" << std::endl;
            return false;
        }

        if (_reconnectFlags[deviceId] && _reconnectFlags[deviceId]->load())
        {
            return true;
        }

        auto chIt = _channels.find(deviceId);
        if (chIt != _channels.end() && chIt->second->IsConnected())
        {
            return true;
        }

        _reconnectFlags[deviceId]->store(true);
        _reconnectAttempts[deviceId]->store(0);

        // 停止旧重连线程并等待其退出（避免 detach 线程访问已销毁对象）
        if (_reconnectThreads.find(deviceId) != _reconnectThreads.end())
        {
            if (_reconnectThreads[deviceId].joinable())
            {
                _reconnectFlags[deviceId]->store(false);
                _channelsMutex.unlock();
                _reconnectThreads[deviceId].join();
                _channelsMutex.lock();
                _reconnectFlags[deviceId]->store(true);
            }
        }

        _reconnectThreads[deviceId] = std::thread(&DeviceManager::ReconnectLoop, this, deviceId);

        std::cout << "Started reconnect thread for device: " << deviceId << std::endl;
        return true;
    }

    void DeviceManager::StopAll()
    {
        _stopping.store(true);
        *_stoppingFlag = true;

        for (auto& pair : _reconnectFlags)
        {
            if (pair.second)
            {
                pair.second->store(false);
            }
        }

        for (auto& pair : _reconnectThreads)
        {
            if (pair.second.joinable())
            {
                pair.second.join();
            }
        }
        _reconnectThreads.clear();

        // 断开所有通道
        std::lock_guard<std::mutex> lock(_channelsMutex);
        for (auto& pair : _channels)
        {
            pair.second->Disconnect();
        }
        _channels.clear();
    }

    std::map<std::string, bool> DeviceManager::GetDeviceStatuses() const
    {
        std::lock_guard<std::mutex> lock(_channelsMutex);
        std::map<std::string, bool> statuses;
        for (const auto& pair : _deviceConfigs)
        {
            const std::string& deviceId = pair.first;
            auto it = _channels.find(deviceId);
            bool connected = (it != _channels.end()) && it->second->IsConnected();
            statuses[deviceId] = connected;
        }
        return statuses;
    }

    bool DeviceManager::IsDeviceConnected(const std::string& deviceId) const
    {
        std::lock_guard<std::mutex> lock(_channelsMutex);
        auto it = _channels.find(deviceId);
        return (it != _channels.end()) && it->second->IsConnected();
    }

    bool DeviceManager::ConnectDevice(const DeviceConfig& device)
    {
        auto protoIt = _protocolConfigs.find(device.protocol);
        if (protoIt == _protocolConfigs.end())
        {
            std::cerr << "Protocol '" << device.protocol << "' not defined" << std::endl;
            return false;
        }

        const ProtocolConfig& proto = protoIt->second;

        try
        {
            auto channel = CreateChannel(proto.framing);
            int port = device.port > 0 ? device.port : proto.transport.defaultPort;

            bool connected = channel->Connect(device.host, port, proto.connection.responseTimeoutMs);
            if (!connected)
            {
                std::cerr << "Device " << device.id << " connection failed" << std::endl;
                return false;
            }

            if (!ExecuteHandshake(channel, proto))
            {
                std::cerr << "Device " << device.id << " handshake failed" << std::endl;
                return false;
            }

            {
                std::lock_guard<std::mutex> lock(_channelsMutex);
                _channels[device.id] = channel;
            }

            std::cout << "Device " << device.id << " connected successfully" << std::endl;
            return true;
        }
        catch (const std::exception& ex)
        {
            std::cerr << "Device " << device.id << " connect error: " << ex.what() << std::endl;
            return false;
        }
    }

    bool DeviceManager::ExecuteHandshake(std::shared_ptr<TcpChannel> channel, const ProtocolConfig& proto)
    {
        if (proto.handshake.empty())
        {
            return true;
        }

        for (const auto& step : proto.handshake)
        {
            std::vector<uint8_t> request = ProtocolEngine::BuildRequest(step.requestTemplate, {});
            std::vector<uint8_t> response;

            const FramingConfig* framing = step.framing ? step.framing.get() : &proto.framing;
            if (!channel->SendReceive(request, response, framing))
            {
                return false;
            }

            if (!Validate(step.validCondition, response))
            {
                return false;
            }
        }

        return true;
    }

    void DeviceManager::ReconnectLoop(const std::string& deviceId)
    {
        auto devIt = _deviceConfigs.find(deviceId);
        if (devIt == _deviceConfigs.end())
        {
            return;
        }

        const DeviceConfig& device = devIt->second;
        auto protoIt = _protocolConfigs.find(device.protocol);
        if (protoIt == _protocolConfigs.end())
        {
            return;
        }

        const ProtocolConfig& proto = protoIt->second;
        int reconnectInterval = proto.connection.reconnectIntervalMs;
        int maxAttempts = proto.connection.maxReconnectAttempts;

        auto stoppingFlag = _stoppingFlag;
        while (_reconnectFlags[deviceId]->load() && !stoppingFlag->load())
        {
            int attempts = _reconnectAttempts[deviceId]->fetch_add(1);
            
            if (maxAttempts > 0 && attempts >= maxAttempts)
            {
                std::cerr << "Device " << deviceId << " max reconnect attempts reached" << std::endl;
                _reconnectFlags[deviceId]->store(false);
                return;
            }

            std::cout << "Reconnect attempt " << attempts + 1 << " for device: " << deviceId << std::endl;

            try
            {
                auto channel = CreateChannel(proto.framing);
                int port = device.port > 0 ? device.port : proto.transport.defaultPort;

                if (channel->Connect(device.host, port, proto.connection.responseTimeoutMs))
                {
                    if (ExecuteHandshake(channel, proto))
                    {
                        {
                            std::lock_guard<std::mutex> lock(_channelsMutex);
                            _channels[deviceId] = channel;
                        }
                        
                        std::cout << "Device " << deviceId << " reconnected successfully" << std::endl;
                        _reconnectFlags[deviceId]->store(false);
                        _reconnectAttempts[deviceId]->store(0);
                        return;
                    }
                }
            }
            catch (const std::exception& ex)
            {
                std::cerr << "Reconnect failed for " << deviceId << ": " << ex.what() << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(reconnectInterval));
        }
    }

    bool DeviceManager::Validate(const std::string& condition, const std::vector<uint8_t>& resp)
    {
        if (condition.empty())
            return true;

        std::regex pattern(R"(resp\[(\d+)\]\s*==\s*(0x[0-9A-Fa-f]+|\d+))");
        std::smatch match;

        if (std::regex_search(condition, match, pattern))
        {
            int idx = std::stoi(match[1]);
            std::string valStr = match[2];
            uint8_t expected;

            if (valStr.substr(0, 2) == "0x")
            {
                expected = static_cast<uint8_t>(std::stoi(valStr.substr(2), nullptr, 16));
            }
            else
            {
                expected = static_cast<uint8_t>(std::stoi(valStr));
            }

            if (static_cast<size_t>(idx) < resp.size())
            {
                return resp[idx] == expected;
            }
        }

        return true;
    }

    std::shared_ptr<TcpChannel> DeviceManager::CreateChannel(const FramingConfig& framing)
    {
        return std::make_shared<TcpChannel>(framing);
    }
}

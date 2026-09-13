#include "TagReader.h"
#include <iostream>

namespace MyProt
{
    TagReader::TagReader(std::shared_ptr<DeviceManager> deviceManager,
        const std::vector<TagDefinition>& tags)
        : _deviceManager(deviceManager)
    {
        for (const auto& tag : tags)
        {
            _tagDict[tag.tagName] = tag;
        }
    }

    bool TagReader::ReadRaw(const std::string& tagName, std::vector<uint8_t>& rawData)
    {
        auto it = _tagDict.find(tagName);
        if (it == _tagDict.end())
        {
            std::cerr << "Tag " << tagName << " not defined" << std::endl;
            return false;
        }

        const TagDefinition& tagDef = it->second;
        const DeviceConfig* device = _deviceManager->GetDevice(tagDef.deviceId);
        if (!device)
        {
            std::cerr << "Device " << tagDef.deviceId << " not found" << std::endl;
            return false;
        }

        const ProtocolConfig* protocol = _deviceManager->GetProtocol(device->protocol);
        if (!protocol)
        {
            std::cerr << "Protocol " << device->protocol << " not found" << std::endl;
            return false;
        }

        auto opIt = protocol->operations.find(tagDef.operation);
        if (opIt == protocol->operations.end())
        {
            std::cerr << "Operation " << tagDef.operation << " not defined" << std::endl;
            return false;
        }

        const OperationConfig& opConfig = opIt->second;

        auto channel = _deviceManager->GetChannel(device->id);
        if (!channel)
        {
            std::cerr << "Channel for device " << device->id << " not available" << std::endl;
            return false;
        }

        std::vector<uint8_t> request = ProtocolEngine::BuildRequest(opConfig.requestTemplate, tagDef.variables);
        return channel->SendReceive(request, rawData);
    }

    bool TagReader::ReadValue(const std::string& tagName, std::vector<uint8_t>& parsedData)
    {
        auto it = _tagDict.find(tagName);
        if (it == _tagDict.end())
        {
            std::cerr << "Tag " << tagName << " not defined" << std::endl;
            return false;
        }

        const TagDefinition& tagDef = it->second;
        const DeviceConfig* device = _deviceManager->GetDevice(tagDef.deviceId);
        if (!device)
        {
            std::cerr << "Device " << tagDef.deviceId << " not found" << std::endl;
            return false;
        }

        const ProtocolConfig* protocol = _deviceManager->GetProtocol(device->protocol);
        if (!protocol)
        {
            std::cerr << "Protocol " << device->protocol << " not found" << std::endl;
            return false;
        }

        auto opIt = protocol->operations.find(tagDef.operation);
        if (opIt == protocol->operations.end())
        {
            std::cerr << "Operation " << tagDef.operation << " not defined" << std::endl;
            return false;
        }

        const OperationConfig& opConfig = opIt->second;

        auto channel = _deviceManager->GetChannel(device->id);
        if (!channel)
        {
            std::cerr << "Channel for device " << device->id << " not available" << std::endl;
            return false;
        }

        // 构建请求并发送
        std::vector<uint8_t> rawData;
        std::vector<uint8_t> request = ProtocolEngine::BuildRequest(opConfig.requestTemplate, tagDef.variables);
        if (!channel->SendReceive(request, rawData))
        {
            return false;
        }

        // 解析响应
        if (!opConfig.responseParser)
        {
            std::cerr << "No response parser configured for operation " << tagDef.operation << std::endl;
            return false;
        }

        try
        {
            parsedData = ProtocolEngine::ParseResponse(rawData, *opConfig.responseParser);
            return true;
        }
        catch (const std::exception& ex)
        {
            std::cerr << "Parse response error: " << ex.what() << std::endl;
            return false;
        }
    }
}

#include "ProtocolConfig.h"
#include "DeviceManager.h"
#include "TagReader.h"
#include "PollingEngine.h"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

using json = nlohmann::json;

namespace MyProt
{
    // JSON 反序列化辅助函数
    ProtocolConfig ParseProtocolConfig(const json& j)
    {
        ProtocolConfig config;
        config.protocolName = j["protocolName"];
        
        if (j.contains("transport"))
        {
            config.transport.type = j["transport"]["type"];
            config.transport.defaultPort = j["transport"]["defaultPort"];
        }

        if (j.contains("connection"))
        {
            config.connection.responseTimeoutMs = j["connection"]["responseTimeoutMs"];
            config.connection.interFrameDelayMs = j["connection"]["interFrameDelayMs"];
            
            if (j["connection"].contains("reconnectIntervalMs"))
                config.connection.reconnectIntervalMs = j["connection"]["reconnectIntervalMs"];
            if (j["connection"].contains("maxReconnectAttempts"))
                config.connection.maxReconnectAttempts = j["connection"]["maxReconnectAttempts"];
        }

        if (j.contains("framing"))
        {
            config.framing.type = j["framing"]["type"];
            
            if (j["framing"].contains("lengthFieldOffset"))
                config.framing.lengthFieldOffset = j["framing"]["lengthFieldOffset"];
            if (j["framing"].contains("lengthFieldLength"))
                config.framing.lengthFieldLength = j["framing"]["lengthFieldLength"];
            if (j["framing"].contains("lengthIncludesHeader"))
                config.framing.lengthIncludesHeader = j["framing"]["lengthIncludesHeader"];
            if (j["framing"].contains("byteOrder"))
                config.framing.byteOrder = j["framing"]["byteOrder"];
            if (j["framing"].contains("headerLength"))
                config.framing.headerLength = j["framing"]["headerLength"];
            if (j["framing"].contains("fixedLength"))
                config.framing.fixedLength = j["framing"]["fixedLength"];
        }

        if (j.contains("operations"))
        {
            for (const auto& opPair : j["operations"].items())
            {
                OperationConfig opConfig;
                
                for (const auto& part : opPair.value()["requestTemplate"])
                {
                    opConfig.requestTemplate.push_back(part);
                }

                if (opPair.value().contains("responseParser"))
                {
                    auto parser = std::make_shared<ResponseParserConfig>();
                    const auto& parserJson = opPair.value()["responseParser"];
                    
                    if (parserJson.contains("validCondition"))
                        parser->validCondition = parserJson["validCondition"];
                    if (parserJson.contains("dataStartIndex"))
                        parser->dataStartIndex = parserJson["dataStartIndex"];
                    if (parserJson.contains("dataLengthExpr"))
                        parser->dataLengthExpr = parserJson["dataLengthExpr"];
                    if (parserJson.contains("valueType"))
                        parser->valueType = parserJson["valueType"];
                    
                    opConfig.responseParser = parser;
                }
                
                config.operations[opPair.key()] = opConfig;
            }
        }

        if (j.contains("handshake"))
        {
            for (const auto& stepJson : j["handshake"])
            {
                HandshakeConfig step;
                step.name = stepJson["name"];
                
                for (const auto& part : stepJson["requestTemplate"])
                {
                    step.requestTemplate.push_back(part);
                }

                if (stepJson.contains("framing"))
                {
                    auto framing = std::make_shared<FramingConfig>();
                    const auto& fj = stepJson["framing"];
                    framing->type = fj["type"];
                    if (fj.contains("lengthFieldOffset"))
                        framing->lengthFieldOffset = fj["lengthFieldOffset"];
                    if (fj.contains("lengthFieldLength"))
                        framing->lengthFieldLength = fj["lengthFieldLength"];
                    if (fj.contains("lengthIncludesHeader"))
                        framing->lengthIncludesHeader = fj["lengthIncludesHeader"];
                    if (fj.contains("byteOrder"))
                        framing->byteOrder = fj["byteOrder"];
                    if (fj.contains("headerLength"))
                        framing->headerLength = fj["headerLength"];
                    if (fj.contains("fixedLength"))
                        framing->fixedLength = fj["fixedLength"];
                    step.framing = framing;
                }

                if (stepJson.contains("validCondition"))
                    step.validCondition = stepJson["validCondition"];
                
                config.handshake.push_back(step);
            }
        }

        return config;
    }

    ConfigRoot ParseTagsConfig(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            throw std::runtime_error("Failed to open tags config: " + filePath);
        }

        json j;
        file >> j;

        ConfigRoot root;

        if (j.contains("Devices"))
        {
            for (const auto& devJson : j["Devices"])
            {
                DeviceConfig dev;
                dev.id = devJson["id"];
                dev.protocol = devJson["protocol"];
                dev.host = devJson["host"];
                if (devJson.contains("port"))
                    dev.port = devJson["port"];
                root.devices.push_back(dev);
            }
        }

        if (j.contains("Tags"))
        {
            for (const auto& tagJson : j["Tags"])
            {
                TagDefinition tag;
                tag.tagName = tagJson["tagName"];
                tag.deviceId = tagJson["deviceId"];
                tag.operation = tagJson["operation"];
                
                if (tagJson.contains("scanRateMs"))
                    tag.scanRateMs = tagJson["scanRateMs"];
                else
                    tag.scanRateMs = 1000;

                if (tagJson.contains("variables"))
                {
                    for (const auto& varPair : tagJson["variables"].items())
                    {
                        tag.variables[varPair.key()] = varPair.value();
                    }
                }
                
                root.tags.push_back(tag);
            }
        }

        return root;
    }
}

void PrintTagValue(const MyProt::TagValue& value)
{
    std::cout << "[" << value.timestamp << "] " 
              << value.tagName << ": ";
    
    if (value.quality == MyProt::TagValue::QualityCode::Good)
    {
        std::cout << "Good (";
        for (size_t i = 0; i < value.rawData.size(); ++i)
        {
            std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0') 
                      << static_cast<int>(value.rawData[i]);
            if (i < value.rawData.size() - 1)
                std::cout << " ";
        }
        std::cout << ")" << std::dec;
    }
    else
    {
        std::cout << "Bad";
    }
    std::cout << std::endl;
}

int main()
{
    try
    {
        std::cout << "=== MyProt C++ Gateway Starting ===" << std::endl;

        // 加载协议配置（自动扫描 protocols/ 目录下所有 JSON 文件）
        std::map<std::string, MyProt::ProtocolConfig> protocols;
        
#ifdef _WIN32
        {
            WIN32_FIND_DATAA fd;
            HANDLE hFind = FindFirstFileA("protocols\\*.json", &fd);
            if (hFind != INVALID_HANDLE_VALUE)
            {
                do {
                    std::string fileName = fd.cFileName;
                    std::string filePath = "protocols/" + fileName;
                    std::ifstream protoFile(filePath);
                    if (protoFile.is_open())
                    {
                        json j;
                        protoFile >> j;
                        auto config = MyProt::ParseProtocolConfig(j);
                        protocols[config.protocolName] = config;
                        std::cout << "Loaded protocol: " << config.protocolName << " from " << fileName << std::endl;
                    }
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
            }
        }
#else
        // 加载 ModbusTCP 协议
        {
            std::ifstream protoFile("protocols/ModbusTCP.json");
            if (protoFile.is_open())
            {
                json j;
                protoFile >> j;
                auto config = MyProt::ParseProtocolConfig(j);
                protocols[config.protocolName] = config;
                std::cout << "Loaded protocol: " << config.protocolName << std::endl;
            }
        }
        // 加载 SEER 协议
        {
            std::ifstream protoFile("protocols/SEER.json");
            if (protoFile.is_open())
            {
                json j;
                protoFile >> j;
                auto config = MyProt::ParseProtocolConfig(j);
                protocols[config.protocolName] = config;
                std::cout << "Loaded protocol: " << config.protocolName << std::endl;
            }
        }
#endif

        // 加载设备和标签配置
        MyProt::ConfigRoot configRoot = MyProt::ParseTagsConfig("tags.json");
        std::cout << "Loaded " << configRoot.devices.size() << " devices" << std::endl;
        std::cout << "Loaded " << configRoot.tags.size() << " tags" << std::endl;

        // 创建设备管理器
        auto deviceManager = std::make_shared<MyProt::DeviceManager>(
            configRoot.devices, protocols);

        // 连接所有设备
        std::cout << "\nConnecting devices..." << std::endl;
        deviceManager->ConnectAll();

        // 创建标签读取器
        auto tagReader = std::make_shared<MyProt::TagReader>(deviceManager, configRoot.tags);

        // 创建轮询引擎
        MyProt::PollingEngine pollingEngine(deviceManager, configRoot.tags, tagReader);

        // 设置数据接收回调
        pollingEngine.OnDataReceived = [](const MyProt::TagValue& value) {
            PrintTagValue(value);
        };

        // 启动轮询
        std::cout << "\nStarting polling engine..." << std::endl;
        pollingEngine.Start();

        // 保持运行
        std::cout << "\nPress Enter to stop..." << std::endl;
        std::cin.get();

        // 停止
        std::cout << "\nStopping..." << std::endl;
        pollingEngine.Stop();
        deviceManager->StopAll();

        std::cout << "=== MyProt C++ Gateway Stopped ===" << std::endl;
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Error: " << ex.what() << std::endl;
        return 1;
    }
}

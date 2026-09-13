#include "PollingEngine.h"
#include <iostream>
#include <map>
#include <chrono>

namespace MyProt
{
    PollingEngine::PollingEngine(std::shared_ptr<DeviceManager> deviceManager,
        const std::vector<TagDefinition>& tags,
        std::shared_ptr<TagReader> tagReader)
        : _deviceManager(deviceManager),
        _tags(tags),
        _tagReader(tagReader)
    {
    }

    PollingEngine::~PollingEngine()
    {
        Stop();
    }

    void PollingEngine::Start()
    {
        if (_running.load())
        {
            return;
        }

        _running.store(true);

        // 按扫描周期分组
        std::map<int, std::vector<TagDefinition>> groups;
        for (const auto& tag : _tags)
        {
            if (tag.scanRateMs > 0)
            {
                groups[tag.scanRateMs].push_back(tag);
            }
        }

        if (groups.empty())
        {
            std::cerr << "No tags to poll (scanRateMs <= 0)" << std::endl;
            return;
        }

        // 为每个扫描周期启动线程
        for (const auto& pair : groups)
        {
            _pollingThreads.push_back(std::thread(&PollingEngine::PollGroup, this, pair.first, pair.second));
        }

        std::cout << "Polling engine started with " << groups.size() << " groups" << std::endl;
    }

    void PollingEngine::Stop()
    {
        _running.store(false);

        for (auto& thread : _pollingThreads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        _pollingThreads.clear();

        std::cout << "Polling engine stopped" << std::endl;
    }

    void PollingEngine::PollGroup(int intervalMs, const std::vector<TagDefinition>& tags)
    {
        std::cout << "Starting poll group: interval " << intervalMs << "ms, tags: " << tags.size() << std::endl;

        while (_running.load())
        {
            auto start = std::chrono::steady_clock::now();

            // 按设备分组
            std::map<std::string, std::vector<TagDefinition>> deviceGroups;
            for (const auto& tag : tags)
            {
                deviceGroups[tag.deviceId].push_back(tag);
            }

            // 对每个设备串行采集
            for (const auto& devicePair : deviceGroups)
            {
                const std::string& deviceId = devicePair.first;
                const std::vector<TagDefinition>& deviceTags = devicePair.second;

                // 检查设备连接状态，离线则跳过该设备所有标签
                if (!_deviceManager->IsDeviceConnected(deviceId))
                {
                    // 设备离线，直接跳过该设备所有标签
                    continue;
                }

                for (const auto& tag : deviceTags)
                {
                    if (!_running.load())
                        return;

                    try
                    {
                        std::vector<uint8_t> parsedData;
                        bool success = _tagReader->ReadValue(tag.tagName, parsedData);

                        TagValue tagValue;
                        tagValue.tagName = tag.tagName;
                        tagValue.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

                        if (success)
                        {
                            tagValue.rawData = parsedData;
                            tagValue.quality = TagValue::QualityCode::Good;
                        }
                        else
                        {
                            tagValue.quality = TagValue::QualityCode::Bad;
                        }

                        // 触发回调
                        if (OnDataReceived)
                        {
                            OnDataReceived(tagValue);
                        }
                    }
                    catch (const std::exception& ex)
                    {
                        std::cerr << "Polling " << tag.tagName << " failed: " << ex.what() << std::endl;

                        TagValue tagValue;
                        tagValue.tagName = tag.tagName;
                        tagValue.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        tagValue.quality = TagValue::QualityCode::Bad;

                        if (OnDataReceived)
                        {
                            OnDataReceived(tagValue);
                        }
                    }
                }
            }

            // 等待到下一个周期
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);

            int sleepMs = intervalMs - static_cast<int>(elapsed.count());
            if (sleepMs > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
        }
    }
}

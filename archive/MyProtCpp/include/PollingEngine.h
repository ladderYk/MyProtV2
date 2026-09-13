#pragma once

#include "ProtocolConfig.h"
#include "DeviceManager.h"
#include "TagReader.h"
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <map>
#include <functional>

namespace MyProt
{
    class PollingEngine
    {
    public:
        using DataReceivedCallback = std::function<void(const TagValue&)>;

        PollingEngine(std::shared_ptr<DeviceManager> deviceManager,
                      const std::vector<TagDefinition>& tags,
                      std::shared_ptr<TagReader> tagReader);
        ~PollingEngine();

        /// <summary>
        /// 启动轮询
        /// </summary>
        void Start();

        /// <summary>
        /// 停止轮询
        /// </summary>
        void Stop();

        /// <summary>
        /// 数据接收事件
        /// </summary>
        DataReceivedCallback OnDataReceived;

    private:
        std::shared_ptr<DeviceManager> _deviceManager;
        std::vector<TagDefinition> _tags;
        std::shared_ptr<TagReader> _tagReader;
        std::vector<std::thread> _pollingThreads;
        std::atomic<bool> _running{ false };

        /// <summary>
        /// 单个轮询组线程函数
        /// </summary>
        void PollGroup(int intervalMs, const std::vector<TagDefinition>& tags);
    };
}

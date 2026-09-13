#pragma once

#include "ProtocolConfig.h"
#include "DeviceManager.h"
#include <memory>
#include <map>
#include <string>

namespace MyProt
{
    class TagReader
    {
    public:
        TagReader(std::shared_ptr<DeviceManager> deviceManager, 
                  const std::vector<TagDefinition>& tags);

        /// <summary>
        /// 读取单个标签的原始响应字节
        /// </summary>
        bool ReadRaw(const std::string& tagName, std::vector<uint8_t>& rawData);

        /// <summary>
        /// 读取标签并解析为最终值
        /// </summary>
        bool ReadValue(const std::string& tagName, std::vector<uint8_t>& parsedData);

    private:
        std::shared_ptr<DeviceManager> _deviceManager;
        std::map<std::string, TagDefinition> _tagDict;
    };
}

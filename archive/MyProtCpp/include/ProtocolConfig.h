#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include <stdexcept>

namespace MyProt
{
    // VS2015 兼容的 Optional<T> 替代 std::optional (C++17)
    template<typename T>
    class Optional
    {
    public:
        Optional() : _hasValue(false), _value() {}
        Optional(const T& val) : _hasValue(true), _value(val) {}

        bool has_value() const { return _hasValue; }

        T value() const
        {
            if (!_hasValue)
                throw std::runtime_error("Optional has no value");
            return _value;
        }

        T value_or(const T& defaultVal) const
        {
            return _hasValue ? _value : defaultVal;
        }

        Optional& operator=(const T& val)
        {
            _value = val;
            _hasValue = true;
            return *this;
        }

        explicit operator bool() const { return _hasValue; }

    private:
        bool _hasValue;
        T _value;
    };

    // 传输配置
    struct TransportConfig
    {
        std::string type;        // "Tcp", "Serial"
        int defaultPort = 502;
    };

    // 连接配置
    struct ConnectionConfig
    {
        int responseTimeoutMs = 1000;
        int interFrameDelayMs = 0;
        int reconnectIntervalMs = 5000;    // 重连间隔
        int maxReconnectAttempts = -1;     // -1表示无限重试
    };

    // 响应解析配置
    struct ResponseParserConfig
    {
        std::string validCondition;
        int dataStartIndex = 0;
        std::string dataLengthExpr;
        std::string valueType;   // "ByteArray", "UInt16", "Empty"
    };

    // 操作配置
    struct OperationConfig
    {
        std::vector<std::string> requestTemplate;
        std::shared_ptr<ResponseParserConfig> responseParser;
    };

    // 帧解析配置
    struct FramingConfig
    {
        std::string type;                         // "LengthField", "Fixed"
        Optional<int> lengthFieldOffset;
        Optional<int> lengthFieldLength;
        Optional<bool> lengthIncludesHeader;
        std::string byteOrder;                    // "BigEndian", "LittleEndian"
        Optional<int> headerLength;               // 仅当 !LengthIncludesHeader
        Optional<int> fixedLength;                // Fixed 模式
    };

    // 握手配置
    struct HandshakeConfig
    {
        std::string name;
        std::vector<std::string> requestTemplate;
        std::shared_ptr<FramingConfig> framing;
        std::string validCondition;
    };

    // 协议配置
    struct ProtocolConfig
    {
        std::string protocolName;
        TransportConfig transport;
        ConnectionConfig connection;
        std::map<std::string, OperationConfig> operations;
        std::vector<std::string> builtInFunctions;
        std::vector<HandshakeConfig> handshake;
        FramingConfig framing;
    };

    // 设备配置
    struct DeviceConfig
    {
        std::string id;
        std::string protocol;
        std::string host;
        int port = 0;
    };

    // 标签定义
    struct TagDefinition
    {
        std::string tagName;
        std::string deviceId;
        std::string operation;
        std::map<std::string, int> variables;
        int scanRateMs = 1000;
    };

    // 标签值
    struct TagValue
    {
        std::string tagName;
        std::vector<uint8_t> rawData;
        uint64_t timestamp = 0;
        enum class QualityCode { Good, Bad } quality;
    };

    // 配置根
    struct ConfigRoot
    {
        std::vector<DeviceConfig> devices;
        std::vector<TagDefinition> tags;
    };
}

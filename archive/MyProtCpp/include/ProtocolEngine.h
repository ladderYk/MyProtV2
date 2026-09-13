#pragma once

#include "ProtocolConfig.h"
#include <vector>
#include <string>
#include <map>
#include <cstdint>
#include <atomic>

namespace MyProt
{
    class ProtocolEngine
    {
    public:
        /// <summary>
        /// 根据模板和变量构建请求字节数组
        /// </summary>
        static std::vector<uint8_t> BuildRequest(
            const std::vector<std::string>& templateParts,
            const std::map<std::string, int>& variables);

        /// <summary>
        /// 解析响应字节数组，返回原始数据
        /// </summary>
        static std::vector<uint8_t> ParseResponse(
            const std::vector<uint8_t>& response,
            const ResponseParserConfig& parser);

    private:
        static std::atomic<int> _transactionId;

        /// <summary>
        /// 格式化变量为字节数组
        /// </summary>
        static std::vector<uint8_t> FormatVariable(int value, const std::string& format);

        /// <summary>
        /// 十六进制字符串转字节数组
        /// </summary>
        static std::vector<uint8_t> HexToBytes(const std::string& hex);

        /// <summary>
        /// 评估条件表达式
        /// </summary>
        static bool EvaluateCondition(const std::string& condition, const std::vector<uint8_t>& resp);

        /// <summary>
        /// 评估表达式
        /// </summary>
        static int EvaluateExpression(const std::string& expr, const std::vector<uint8_t>& resp);
    };
}

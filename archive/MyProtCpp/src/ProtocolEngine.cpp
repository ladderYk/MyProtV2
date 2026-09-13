#include "ProtocolEngine.h"
#include <sstream>
#include <regex>
#include <algorithm>
#include <stdexcept>
#include <cctype>

namespace MyProt
{
    std::atomic<int> ProtocolEngine::_transactionId{0};

    std::vector<uint8_t> ProtocolEngine::BuildRequest(
        const std::vector<std::string>& templateParts,
        const std::map<std::string, int>& variables)
    {
        struct Segment
        {
            std::vector<uint8_t> data;
            bool isCalcLength;

            Segment() : isCalcLength(false) {}
            Segment(const std::vector<uint8_t>& d, bool calc) : data(d), isCalcLength(calc) {}
        };

        std::vector<Segment> segments;

        for (const auto& part : templateParts)
        {
            std::string trimmed = part;
            trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(),
                [](unsigned char ch) { return !std::isspace(ch); }));
            trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(),
                [](unsigned char ch) { return !std::isspace(ch); }).base(), trimmed.end());

            if (trimmed.empty())
                continue;

            // 字符串模板：$ 前缀表示原始字符串（用于 JSON 等文本数据）
            // 支持 $(VarName) 变量替换
            if (trimmed[0] == '$')
            {
                std::string strContent = trimmed.substr(1);

                // 替换 $(VarName) 为变量值
                {   
                    std::string::size_type pos = 0;
                    while ((pos = strContent.find("$(", pos)) != std::string::npos)
                    {
                        std::string::size_type end = strContent.find(')', pos + 2);
                        if (end == std::string::npos) break;

                        std::string varName = strContent.substr(pos + 2, end - pos - 2);
                        auto it = variables.find(varName);
                        if (it != variables.end())
                        {
                            std::string valueStr = std::to_string(it->second);
                            strContent.replace(pos, end - pos + 1, valueStr);
                            pos += valueStr.size();
                        }
                        else
                        {
                            throw std::runtime_error("Variable not found in string template: " + varName);
                        }
                }

                std::vector<uint8_t> strBytes(strContent.begin(), strContent.end());
                segments.push_back(Segment(strBytes, false));
            }
            else if (trimmed.front() == '{' && trimmed.back() == '}')
            {
                std::string content = trimmed.substr(1, trimmed.size() - 2);
                std::vector<std::string> parts;
                std::stringstream ss(content);
                std::string token;
                while (std::getline(ss, token, ':'))
                {
                    parts.push_back(token);
                }

                std::string varName = parts[0];
                std::string func;
                std::string format;

                if (parts.size() == 3)
                {
                    func = parts[1];
                    format = parts[2];
                }
                else if (parts.size() == 2)
                {
                    format = parts[1];
                }

                if (func == "auto" && varName == "TransactionID")
                {
                    int tid = ++_transactionId;
                    std::vector<uint8_t> tidBytes(2);
                    tidBytes[0] = static_cast<uint8_t>((tid >> 8) & 0xFF);
                    tidBytes[1] = static_cast<uint8_t>(tid & 0xFF);
                    segments.push_back({ tidBytes, false });
                }
                else if (func == "calc" && varName == "Length")
                {
                    // format 指定长度字段字节数：X4=2字节, X8=4字节
                    int calcLenBytes = 2;
                    if (format == "X8") calcLenBytes = 4;
                    else if (format == "X4") calcLenBytes = 2;
                    segments.push_back(Segment(std::vector<uint8_t>(calcLenBytes, 0), true));
                }
                else
                {
                    auto it = variables.find(varName);
                    if (it != variables.end())
                    {
                        std::vector<uint8_t> valueBytes = FormatVariable(it->second, format);
                        segments.push_back({ valueBytes, false });
                    }
                    else
                    {
                        throw std::runtime_error("Variable not found: " + varName);
                    }
                }
            }
            else
            {
                std::string hex = trimmed;
                hex.erase(std::remove(hex.begin(), hex.end(), ' '), hex.end());
                std::vector<uint8_t> bytes = HexToBytes(hex);
                segments.push_back({ bytes, false });
            }
        }

        int totalLengthAfterLengthField = 0;
        int calcIndex = -1;
        for (size_t i = 0; i < segments.size(); ++i)
        {
            if (segments[i].isCalcLength)
            {
                calcIndex = static_cast<int>(i);
                totalLengthAfterLengthField = 0;
            }
            else if (calcIndex >= 0)
            {
                totalLengthAfterLengthField += static_cast<int>(segments[i].data.size());
            }
        }

        if (calcIndex >= 0)
        {
            int calcLenBytes = static_cast<int>(segments[calcIndex].data.size());
            std::vector<uint8_t> lenBytes(calcLenBytes, 0);
            for (int i = 0; i < calcLenBytes; ++i)
            {
                lenBytes[calcLenBytes - 1 - i] = static_cast<uint8_t>((totalLengthAfterLengthField >> (8 * i)) & 0xFF);
            }
            segments[calcIndex] = Segment(lenBytes, false);
        }

        std::vector<uint8_t> result;
        for (const auto& seg : segments)
        {
            if (!seg.isCalcLength && !seg.data.empty())
            {
                result.insert(result.end(), seg.data.begin(), seg.data.end());
            }
        }

        return result;
    }

    std::vector<uint8_t> ProtocolEngine::ParseResponse(
        const std::vector<uint8_t>& response,
        const ResponseParserConfig& parser)
    {
        if (!parser.validCondition.empty())
        {
            if (!EvaluateCondition(parser.validCondition, response))
            {
                throw std::runtime_error("Response invalid: " + parser.validCondition);
            }
        }

        if (parser.valueType == "Empty")
        {
            return {};
        }

        // dataLengthExpr 为空或 "*" 表示提取从 dataStartIndex 到末尾的全部数据
        int dataLen;
        if (parser.dataLengthExpr.empty() || parser.dataLengthExpr == "*")
        {
            dataLen = static_cast<int>(response.size()) - parser.dataStartIndex;
        }
        else
        {
            dataLen = EvaluateExpression(parser.dataLengthExpr, response);
        }
        if (dataLen <= 0)
        {
            return {};
        }

        if (static_cast<size_t>(parser.dataStartIndex + dataLen) > response.size())
        {
            throw std::runtime_error("Response data out of range");
        }

        std::vector<uint8_t> data(response.begin() + parser.dataStartIndex,
            response.begin() + parser.dataStartIndex + dataLen);

        return data;
    }

    std::vector<uint8_t> ProtocolEngine::FormatVariable(int value, const std::string& format)
    {
        int byteCount = 1;
        if (format == "X4")
            byteCount = 2;
        else if (format == "X8")
            byteCount = 4;

        std::vector<uint8_t> bytes(byteCount);
        for (int i = 0; i < byteCount; ++i)
        {
            bytes[byteCount - 1 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
        }
        return bytes;
    }

    std::vector<uint8_t> ProtocolEngine::HexToBytes(const std::string& hex)
    {
        if (hex.size() % 2 != 0)
        {
            throw std::runtime_error("Invalid hex string (odd length): " + hex);
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(hex.size() / 2);

        for (size_t i = 0; i < hex.size(); i += 2)
        {
            std::string byteStr = hex.substr(i, 2);
            uint8_t byte = static_cast<uint8_t>(std::stoi(byteStr, nullptr, 16));
            bytes.push_back(byte);
        }

        return bytes;
    }

    bool ProtocolEngine::EvaluateCondition(const std::string& condition, const std::vector<uint8_t>& resp)
    {
        std::regex pattern(R"(resp\[(\d+)\]\s*==\s*(0x[0-9a-fA-F]+|\d+))");
        std::smatch match;

        if (std::regex_search(condition, match, pattern))
        {
            int index = std::stoi(match[1]);
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

            if (static_cast<size_t>(index) < resp.size())
            {
                return resp[index] == expected;
            }
        }

        return true;
    }

    int ProtocolEngine::EvaluateExpression(const std::string& expr, const std::vector<uint8_t>& resp)
    {
        if (expr.empty())
            return 0;

        std::regex pattern(R"(resp\[(\d+)\])");
        std::smatch match;

        if (std::regex_search(expr, match, pattern))
        {
            int index = std::stoi(match[1]);
            if (static_cast<size_t>(index) < resp.size())
            {
                return static_cast<int>(resp[index]);
            }
            return 0;
        }

        try
        {
            return std::stoi(expr);
        }
        catch (const std::exception&)
        {
            throw std::runtime_error("Cannot evaluate expression: " + expr);
        }
    }
}

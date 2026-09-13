// src/Transport/include/MyProt/Transport/IFrameParser.hpp
// 帧解析器接口 — 从字节流中提取完整帧 (C++11, ADR-0010 §4)

#pragma once
#include <vector>
#include <cstdint>
#include <memory>
#include "MyProt/Core/ByteView.hpp"
#include "MyProt/Core/Expected.hpp"

namespace MyProt { namespace Transport {

// 接口签名直接使用 Core 层类型; using 声明使派生类实现无需再限定
using Core::ByteView;
using Core::Expected;
using Core::Unexpected;
using Core::Error;

/// 帧解析结果
struct FrameParseResult {
    std::vector<uint8_t> frame;      // 完整帧数据
    size_t consumedBytes;             // 已消耗的字节数
    bool needMoreData;               // 是否需要更多数据
};

/// 帧解析器接口
class IFrameParser {
public:
    virtual ~IFrameParser() = default;

    /// 尝试从缓冲区解析帧
    /// @param data 输入数据缓冲区
    /// @param startPos 开始解析的位置
    /// @return 解析结果，包含帧数据和消耗的字节数
    virtual Expected<FrameParseResult> Parse(const ByteView& data, size_t startPos = 0) = 0;

    /// 检查缓冲区开头是否有完整帧
    /// @param data 输入数据缓冲区
    /// @return true 如果缓冲区开头有完整帧
    virtual bool HasCompleteFrame(const ByteView& data) = 0;
};

/// 帧解析器智能指针
using IFrameParserPtr = std::shared_ptr<IFrameParser>;

}} // namespace MyProt::Transport
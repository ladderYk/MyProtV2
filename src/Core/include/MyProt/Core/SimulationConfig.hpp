// src/Core/include/MyProt/Core/SimulationConfig.hpp
// v1.1 增: 仿真配置 POCO. 原本嵌在 Config.hpp 内, 因 v1.1 移至 server.json 顶层
// 而抽离, 供 Config.hpp (残留引用) + ServerConfig.hpp 共同 include.
//
// 仿真本应隶属"服务端行为"而非"协议语法", 但因字段都是结构化数据, 仍放 Core 层。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace MyProt {
namespace Core {

/// 仿真操作行为描述. 见 docs/modules/04_Service.md §仿真
///   read  → 按匹配变量从数据区取 countVar×2 字节作为应答数据;
///   write → 请求数据区 (dataOffset 起) 写入数据区, 应答不带数据。
struct SimOperationConfig {
    std::string kind;          // "read" | "write"
    std::string addressVar;    // 匹配变量名 → 数据区起始地址
    std::string countVar;      // read: 匹配变量名 → 寄存器数量; write 可空
    int dataOffset;            // write: 请求内数据起始偏移; -1 = 未声明 (忽略写)
    // 自定义应答模板 (可选): 非空时覆盖默认 echo 反向合成。
    // 文法: hex 字面量 ("AA 0B") | {req:N:M} 从请求帧偏移 N 拷贝 M 字节 | {data} 展开数据区。
    std::vector<std::string> responseTemplate;

    SimOperationConfig() : dataOffset(-1) {}
};

/// 全局仿真配置 (v1.1 起放 server.json 而非 protocols/*.json). listenPort == 0 = 不启用仿真服务。
struct SimulationConfig {
    uint16_t listenPort;
    int registerCount;                                    // 数据区寄存器数
    std::map<std::string, uint32_t> initialValues;        // 地址(十进制字符串) → 初值
    std::map<std::string, SimOperationConfig> operations; // 操作名 → 仿真行为

    SimulationConfig() : listenPort(0), registerCount(65536) {}
};

}} // namespace MyProt::Core

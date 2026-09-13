// src/Simulation/include/MyProt/Simulation/SimulationDataStore.hpp
// 仿真数据区 — 16 位寄存器阵列 (HslCommunication DataStore 风格)。
// 仿真服务器的数据来源: 客户端读操作从这里取数, 写操作向这里落数,
// 测试/上位程序也可通过 API 直接预置或修改 (人工创建方式)。

#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace MyProt { namespace Simulation {

class SimulationDataStore {
public:
    explicit SimulationDataStore(std::size_t registerCount);

    /// 单寄存器写入 (预置初值/运行时改值)
    void WriteRegisterValue(std::uint16_t address, std::uint16_t value);

    /// 批量写入 (data 按 2 字节一个寄存器, 大端); 越界返回 false
    bool WriteRegisters(std::uint16_t address, const std::vector<std::uint8_t>& data);

    /// 读取 count 个寄存器 → count*2 字节大端; 越界返回空
    std::vector<std::uint8_t> ReadRegisters(std::uint16_t address,
                                            std::uint16_t count) const;

    std::size_t Size() const;

private:
    mutable std::mutex _mutex;
    std::vector<std::uint16_t> _regs;
};

}} // namespace MyProt::Simulation

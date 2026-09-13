// src/Simulation/src/SimulationDataStore.cpp
// 仿真数据区实现 — 见 SimulationDataStore.hpp 文件头注释

#include "MyProt/Simulation/SimulationDataStore.hpp"

namespace MyProt { namespace Simulation {

SimulationDataStore::SimulationDataStore(std::size_t registerCount)
    : _regs(registerCount, 0) {
}

void SimulationDataStore::WriteRegisterValue(std::uint16_t address, std::uint16_t value) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (address < _regs.size()) _regs[address] = value;
}

bool SimulationDataStore::WriteRegisters(std::uint16_t address,
                                         const std::vector<std::uint8_t>& data) {
    if (data.size() % 2 != 0) return false;
    const std::size_t count = data.size() / 2;

    std::lock_guard<std::mutex> lock(_mutex);
    if (static_cast<std::size_t>(address) + count > _regs.size()) return false;
    for (std::size_t i = 0; i < count; ++i) {
        _regs[address + i] = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(data[i * 2]) << 8) | data[i * 2 + 1]);
    }
    return true;
}

std::vector<std::uint8_t> SimulationDataStore::ReadRegisters(std::uint16_t address,
                                                             std::uint16_t count) const {
    std::vector<std::uint8_t> out;

    std::lock_guard<std::mutex> lock(_mutex);
    if (static_cast<std::size_t>(address) + count > _regs.size()) return out;
    out.reserve(static_cast<std::size_t>(count) * 2);
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint16_t v = _regs[address + i];
        out.push_back(static_cast<std::uint8_t>(v >> 8));
        out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    return out;
}

std::size_t SimulationDataStore::Size() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _regs.size();
}

}} // namespace MyProt::Simulation

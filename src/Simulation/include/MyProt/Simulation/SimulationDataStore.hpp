// src/Simulation/include/MyProt/Simulation/SimulationDataStore.hpp
// Simulation data region - a 16-bit register array (HslCommunication DataStore style).
// The simulation server's data source: client read operations take data from here, write operations land data here,
// and test/host programs can also preset or modify it directly via the API (manual authoring).

#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace MyProt { namespace Simulation {

class SimulationDataStore {
public:
    explicit SimulationDataStore(std::size_t registerCount);

    /// Single-register write (preset initial value / change value at runtime)
    void WriteRegisterValue(std::uint16_t address, std::uint16_t value);

    /// Batch write (data is one register per 2 bytes, big-endian); returns false if out of range
    bool WriteRegisters(std::uint16_t address, const std::vector<std::uint8_t>& data);

    /// Read count registers -> count*2 bytes big-endian; returns empty if out of range
    std::vector<std::uint8_t> ReadRegisters(std::uint16_t address,
                                            std::uint16_t count) const;

    std::size_t Size() const;

private:
    mutable std::mutex _mutex;
    std::vector<std::uint16_t> _regs;
};

}} // namespace MyProt::Simulation

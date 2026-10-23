// src/Core/include/MyProt/Core/SimulationConfig.hpp
// Simulation config POCO. One instance per simulator, configured either in a protocol file
// (`protocols/*.json` -> ProtocolConfig.simulation, multi-device path) or at the top level of
// server.json (ServerConfig.simulation, legacy single-instance fallback).
//
// The data area is protocol-agnostic (a 16-bit register array); frame knowledge still comes
// from the owning protocol's transport/framing/operations.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace MyProt {
namespace Core {

/// Simulation operation behavior description. See docs/modules/04_Service.md §simulation
///   read  → fetch countVar×2 bytes from the data area per matched variable as the response data;
///   write → write the request data area (from dataOffset) into the data area; response carries no data.
struct SimOperationConfig {
    std::string kind;          // "read" | "write"
    std::string addressVar;    // matched variable name -> data-area start address
    std::string countVar;      // read: matched variable name -> register count; write may be empty
    int dataOffset;            // write: data start offset within the request; -1 = undeclared (ignore the write)
    // Custom response template (optional): when non-empty, overrides the default echo reverse-synthesis.
    // Grammar: hex literal ("AA 0B") | {req:N:M} copy M bytes from request-frame offset N | {data} expand the data area.
    std::vector<std::string> responseTemplate;

    SimOperationConfig() : dataOffset(-1) {}
};

/// Simulation instance config. listenPort == 0 = this instance is disabled.
struct SimulationConfig {
    uint16_t listenPort;
    int registerCount;                                    // number of data-area registers
    std::map<std::string, uint32_t> initialValues;        // address (decimal string) -> initial value
    std::map<std::string, SimOperationConfig> operations; // operation name -> simulation behavior

    SimulationConfig() : listenPort(0), registerCount(65536) {}
};

}} // namespace MyProt::Core

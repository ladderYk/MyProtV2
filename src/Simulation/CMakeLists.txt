# src/Simulation/CMakeLists.txt
# Simulation 层构建配置 (C++11, ADR-0010 §5, Phase 1 已落地)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Simulation)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Engine/include)

set(SIM_SOURCES
    src/SimulationServer.cpp
    src/ResponseSynthesizer.cpp
    src/SimulationDataStore.cpp
    src/TemplateMatcher.cpp
)

set(SIM_HEADERS
    include/MyProt/Simulation/SimulationServer.hpp
    include/MyProt/Simulation/ResponseSynthesizer.hpp
    include/MyProt/Simulation/SimulationDataStore.hpp
    include/MyProt/Simulation/TemplateMatcher.hpp
)

add_library(MyProt.Simulation STATIC ${SIM_SOURCES} ${SIM_HEADERS})

set_target_properties(MyProt.Simulation PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_link_libraries(MyProt.Simulation
    PRIVATE
    MyProt.Core
    MyProt.Engine
)

target_compile_definitions(MyProt.Simulation PRIVATE
    UNICODE
    _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProt.Simulation PRIVATE DEBUG=1)
endif()

install(TARGETS MyProt.Simulation
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${SIM_HEADERS}
    DESTINATION include/MyProt/Simulation
)

# src/App/CMakeLists.txt
# App 入口层构建配置 (C++11, ADR-0010 §5, 生产 main + RuntimeGlue + E2E)

cmake_minimum_required(VERSION 3.10)
project(MyProt.App)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Engine/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Transport/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Gateway/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Polling/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Service/include)
include_directories(${CMAKE_SOURCE_DIR}/src/WebApi/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Simulation/include)

find_package(asio REQUIRED)

# 生产入口 (MyProtApp.exe)
add_executable(MyProtApp src/main.cpp src/RuntimeGlue.cpp)

target_link_libraries(MyProtApp
    PRIVATE
    MyProt.Core
    MyProt.Engine
    MyProt.Transport
    MyProt.Gateway
    MyProt.Polling
    MyProt.Service
    MyProt.WebApi
    MyProt.Simulation
    asio::asio
)

target_compile_definitions(MyProtApp PRIVATE
    UNICODE
    _UNICODE
)

# E2E 测试入口 (MyProt.E2E.exe, 独立进程)
add_executable(MyProt.E2E ../Tests/E2EMain.cpp)

target_link_libraries(MyProt.E2E
    PRIVATE
    MyProt.Core
    MyProt.Engine
    MyProt.Transport
    MyProt.Gateway
    MyProt.Polling
    MyProt.Service
    MyProt.WebApi
    MyProt.Simulation
    asio::asio
)

target_compile_definitions(MyProt.E2E PRIVATE
    UNICODE
    _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProtApp  PRIVATE DEBUG=1)
    target_compile_definitions(MyProt.E2E PRIVATE DEBUG=1)
endif()

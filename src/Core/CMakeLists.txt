# src/Core/CMakeLists.txt
# Core 层构建配置 (C++11, ADR-0010 §5, header-only)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Core)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录 (header-only, 仅 IDE 集成)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# 头文件 (无 .cpp 编译单元, MSVC/CMake 均允许空 STATIC)
set(CORE_HEADERS
    include/MyProt/Core/Expected.hpp
    include/MyProt/Core/ByteView.hpp
    include/MyProt/Core/ByteOrder.hpp
    include/MyProt/Core/Config.hpp
    include/MyProt/Core/Value.hpp
    include/MyProt/Core/Log.hpp
    include/MyProt/Core/Metrics.hpp
)

add_library(MyProt.Core INTERFACE)
target_include_directories(MyProt.Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 编译选项
target_compile_features(MyProt.Core INTERFACE cxx_std_11)

# 安装规则
install(DIRECTORY include/MyProt
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

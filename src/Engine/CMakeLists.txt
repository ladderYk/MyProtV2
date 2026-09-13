# src/Engine/CMakeLists.txt
# Engine 层构建配置 (C++11, ADR-0010 §5)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Engine)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Transport/include)

# 查找依赖
find_package(asio REQUIRED)

# 源文件
set(ENGINE_SOURCES
    src/ExpressionEvaluator.cpp
    src/RequestBuilder.cpp
    src/ResponseParser.cpp
    src/AutoComputeProvider.cpp
    src/MiniExpression.cpp
)

# 头文件
set(ENGINE_HEADERS
    include/MyProt/Engine/RequestBuilder.hpp
    include/MyProt/Engine/ResponseParser.hpp
)

# 创建静态库
add_library(MyProt.Engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})

# 设置输出目录
set_target_properties(MyProt.Engine PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 依赖关系
target_link_libraries(MyProt.Engine
    PRIVATE
    MyProt.Core
    MyProt.Transport
    asio::asio
)

# 编译选项
target_compile_definitions(MyProt.Engine PRIVATE
    UNICODE
    _UNICODE
)

# 调试信息
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProt.Engine PRIVATE DEBUG=1)
endif()

# 安装规则
install(TARGETS MyProt.Engine
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${ENGINE_HEADERS}
    DESTINATION include/MyProt/Engine
)
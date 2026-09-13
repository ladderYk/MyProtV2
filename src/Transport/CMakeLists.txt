# src/Transport/CMakeLists.txt
# Transport 层构建配置 (C++11, ADR-0010 §4)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Transport)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)

# 查找依赖
find_package(asio REQUIRED)

# 源文件
set(TRANSPORT_SOURCES
    src/TcpChannel.cpp
    src/LengthFieldFrameParser.cpp
)

# 头文件
set(TRANSPORT_HEADERS
    include/MyProt/Transport/IChannel.hpp
    include/MyProt/Transport/TcpChannel.hpp
    include/MyProt/Transport/IFrameParser.hpp
    include/MyProt/Transport/LengthFieldFrameParser.hpp
)

# 创建静态库
add_library(MyProt.Transport STATIC ${TRANSPORT_SOURCES} ${TRANSPORT_HEADERS})

# 设置输出目录
set_target_properties(MyProt.Transport PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# 链接依赖
target_link_libraries(MyProt.Transport
    asio
    MyProt.Core  # 依赖 Core 层
)

# 编译选项
if(MSVC)
    target_compile_options(MyProt.Transport PRIVATE
        /W4
        /WX
        /wd4251  # DLL 接口警告
    )
else()
    target_compile_options(MyProt.Transport PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()

# 安装规则
install(TARGETS MyProt.Transport
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${TRANSPORT_HEADERS} 
    DESTINATION include/MyProt/Transport
)
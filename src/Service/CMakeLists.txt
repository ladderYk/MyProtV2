# src/Service/CMakeLists.txt
# Service 层构建配置 (与 MyProt.Service.vcxproj 保持一致)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Service)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Engine/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Gateway/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Polling/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Simulation/include)
include_directories(${CMAKE_SOURCE_DIR}/src/WebApi/include)

# 源文件 (与 vcxproj ClCompile 一致)
set(SERVICE_SOURCES
    src/ConfigStore.cpp
    src/ConfigDeepValidator.cpp
    src/SchemaRegistry.cpp
    src/ConfigDirectoryLoader.cpp
    src/SessionContext.cpp
)

# 头文件 (与 vcxproj ClInclude 一致)
set(SERVICE_HEADERS
    include/MyProt/Service/ConfigStore.hpp
    include/MyProt/Service/ConfigValidator.hpp
    include/MyProt/Service/SchemaRegistry.hpp
    include/MyProt/Service/ConfigDirectoryLoader.hpp
    include/MyProt/Service/SessionContext.hpp
)

# 创建静态库
add_library(MyProt.Service STATIC ${SERVICE_SOURCES} ${SERVICE_HEADERS})

# 设置输出目录
set_target_properties(MyProt.Service PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

# 安装规则
install(TARGETS MyProt.Service
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${SERVICE_HEADERS}
    DESTINATION include/MyProt/Service
)

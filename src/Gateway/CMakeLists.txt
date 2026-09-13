# src/Gateway/CMakeLists.txt
# Gateway 层构建配置 (C++11, ADR-0010 §5)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Gateway)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Transport/include)

find_package(asio REQUIRED)

set(GATEWAY_SOURCES
    src/ProtocolGateway.cpp
    src/ChannelManager.cpp
    src/TagGrouper.cpp
    src/TagReader.cpp
)

set(GATEWAY_HEADERS
    include/MyProt/Gateway/ProtocolGateway.hpp
    include/MyProt/Gateway/ChannelManager.hpp
    include/MyProt/Gateway/TagGrouper.hpp
    include/MyProt/Gateway/TagReader.hpp
    include/MyProt/Gateway/ProtocolLookup.hpp
    include/MyProt/Gateway/MergedRequest.hpp
)

add_library(MyProt.Gateway STATIC ${GATEWAY_SOURCES} ${GATEWAY_HEADERS})

set_target_properties(MyProt.Gateway PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_link_libraries(MyProt.Gateway
    PRIVATE
    MyProt.Core
    MyProt.Transport
    asio::asio
)

target_compile_definitions(MyProt.Gateway PRIVATE
    UNICODE
    _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProt.Gateway PRIVATE DEBUG=1)
endif()

install(TARGETS MyProt.Gateway
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${GATEWAY_HEADERS}
    DESTINATION include/MyProt/Gateway
)

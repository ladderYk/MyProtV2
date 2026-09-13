# src/Polling/CMakeLists.txt
# Polling 层构建配置 (C++11, ADR-0010 §5)

cmake_minimum_required(VERSION 3.10)
project(MyProt.Polling)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Engine/include)

set(POLLING_SOURCES
    src/PollingEngine.cpp
    src/LatestValueStore.cpp
)

set(POLLING_HEADERS
    include/MyProt/Polling/PollingEngine.hpp
    include/MyProt/Polling/LatestValueStore.hpp
    include/MyProt/Polling/PollStats.hpp
)

add_library(MyProt.Polling STATIC ${POLLING_SOURCES} ${POLLING_HEADERS})

set_target_properties(MyProt.Polling PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_link_libraries(MyProt.Polling
    PRIVATE
    MyProt.Core
    MyProt.Engine
)

target_compile_definitions(MyProt.Polling PRIVATE
    UNICODE
    _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProt.Polling PRIVATE DEBUG=1)
endif()

install(TARGETS MyProt.Polling
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${POLLING_HEADERS}
    DESTINATION include/MyProt/Polling
)

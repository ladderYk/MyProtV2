# src/WebApi/CMakeLists.txt
# WebApi 层构建配置 (C++11, ADR-0010 §5)

cmake_minimum_required(VERSION 3.10)
project(MyProt.WebApi)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Core/include)
include_directories(${CMAKE_SOURCE_DIR}/src/Service/include)

set(WEBAPI_SOURCES
    src/WebApiServer.cpp
    src/AuthMiddleware.cpp
)

set(WEBAPI_HEADERS
    include/MyProt/WebApi/WebApiServer.hpp
    include/MyProt/WebApi/AuthMiddleware.hpp
)

add_library(MyProt.WebApi STATIC ${WEBAPI_SOURCES} ${WEBAPI_HEADERS})

set_target_properties(MyProt.WebApi PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_link_libraries(MyProt.WebApi
    PRIVATE
    MyProt.Core
    MyProt.Service
)

target_compile_definitions(MyProt.WebApi PRIVATE
    UNICODE
    _UNICODE
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(MyProt.WebApi PRIVATE DEBUG=1)
endif()

install(TARGETS MyProt.WebApi
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${WEBAPI_HEADERS}
    DESTINATION include/MyProt/WebApi
)

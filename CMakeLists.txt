# MyProt V2 — 跨平台构建入口 (Linux / Windows)
#
# Windows 上仍以 MyProt.sln (MSBuild/v140) 为主, 本文件用于 Linux 与 CI;
# 两者共享同一份源码树与头文件目录, 不引入第二套源码组织。
#
# 用法:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j
#
# 约束: C++11 (ADR-0010), 不使用 C++14/17 特性。

cmake_minimum_required(VERSION 3.13)
project(MyProt VERSION 2.1.0 LANGUAGES CXX)

# 发布版本: 预发布加后缀 (如 "-rc.1"), 正式版留空。
# project() 的 VERSION 只接受纯数字, 带后缀的完整版本只用于包名与发布元数据。
set(MYPROT_VERSION_SUFFIX "-rc.1")
set(MYPROT_VERSION "${PROJECT_VERSION}${MYPROT_VERSION_SUFFIX}")

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MYPROT_BUILD_TESTS "Build unit tests and the E2E runner" ON)

set(MYPROT_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(MYPROT_THIRD_PARTY ${MYPROT_ROOT}/third_party)

find_package(Threads REQUIRED)

# ── 公共编译设置 ──
add_library(myprot_build_flags INTERFACE)
target_include_directories(myprot_build_flags INTERFACE
    ${MYPROT_ROOT}/src/Core/include
    ${MYPROT_ROOT}/src/Engine/include
    ${MYPROT_ROOT}/src/Transport/include
    ${MYPROT_ROOT}/src/Gateway/include
    ${MYPROT_ROOT}/src/Polling/include
    ${MYPROT_ROOT}/src/Service/include
    ${MYPROT_ROOT}/src/Simulation/include
    ${MYPROT_ROOT}/src/WebApi/include
    ${MYPROT_ROOT}/src/Northbound/include
    ${MYPROT_ROOT}/src/Southbound/include
    ${MYPROT_THIRD_PARTY}/asio/include
    ${MYPROT_THIRD_PARTY}
)
target_compile_definitions(myprot_build_flags INTERFACE ASIO_STANDALONE)
target_link_libraries(myprot_build_flags INTERFACE Threads::Threads)

if(WIN32)
    target_compile_definitions(myprot_build_flags INTERFACE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0601
    )
    target_link_libraries(myprot_build_flags INTERFACE ws2_32 mswsock dbghelp)
endif()

# ── 分层静态库 (与 vcxproj 一一对应) ──
add_library(myprot_core STATIC
    src/Core/src/MyProt.Core.ForceLib.cpp
    src/Core/src/TemplateGrammar.cpp
)
target_link_libraries(myprot_core PUBLIC myprot_build_flags)

add_library(myprot_engine STATIC
    src/Engine/src/AutoComputeProvider.cpp
    src/Engine/src/MiniExpression.cpp
    src/Engine/src/RequestBuilder.cpp
    src/Engine/src/ResponseParser.cpp
)
target_link_libraries(myprot_engine PUBLIC myprot_core)

add_library(myprot_transport STATIC
    src/Transport/src/LengthFieldFrameParser.cpp
    src/Transport/src/SerialChannel.cpp
    src/Transport/src/StreamFrameCodec.cpp
    src/Transport/src/TcpChannel.cpp
    src/Transport/src/TlsChannel.cpp
    src/Transport/src/UdpChannel.cpp
)
target_link_libraries(myprot_transport PUBLIC myprot_core)

add_library(myprot_gateway STATIC
    src/Gateway/src/ChannelManager.cpp
    src/Gateway/src/ProtocolGateway.cpp
    src/Gateway/src/TagGrouper.cpp
    src/Gateway/src/TagReader.cpp
    src/Gateway/src/WriteService.cpp
)
target_link_libraries(myprot_gateway PUBLIC myprot_core myprot_engine myprot_transport)

add_library(myprot_polling STATIC
    src/Polling/src/LatestValueStore.cpp
    src/Polling/src/PollingEngine.cpp
)
target_link_libraries(myprot_polling PUBLIC myprot_core myprot_engine myprot_gateway)

add_library(myprot_service STATIC
    src/Service/src/ConfigDeepValidator.cpp
    src/Service/src/ConfigDirectoryLoader.cpp
    src/Service/src/ConfigStore.cpp
    src/Service/src/ProtocolJsonParser.cpp
    src/Service/src/ProtoTemplate.cpp
    src/Service/src/SchemaChecker.cpp
    src/Service/src/SemanticChecker.cpp
    src/Service/src/TrialRenderer.cpp
    src/Service/src/TrialPreview.cpp
    src/Service/src/ResponsePreview.cpp
    src/Service/src/SchemaRegistry.cpp
    src/Service/src/SessionContext.cpp
    src/Service/src/ValidationIssue.cpp
)
target_link_libraries(myprot_service PUBLIC myprot_core myprot_engine)

add_library(myprot_simulation STATIC
    src/Simulation/src/ResponseSynthesizer.cpp
    src/Simulation/src/SimulationDataStore.cpp
    src/Simulation/src/SimulationServer.cpp
    src/Simulation/src/TemplateMatcher.cpp
)
target_link_libraries(myprot_simulation PUBLIC myprot_core myprot_engine myprot_transport)

add_library(myprot_webapi STATIC
    src/WebApi/src/AuthMiddleware.cpp
    src/WebApi/src/WebApiServer.cpp
)
target_link_libraries(myprot_webapi PUBLIC myprot_core myprot_service)

# 北向 MQTT 发布 — 自研 3.1.1 子集 (不引入外部 mqtt 依赖)
add_library(myprot_northbound STATIC
    src/Northbound/src/MqttCodec.cpp
    src/Northbound/src/MqttSession.cpp
    src/Northbound/src/MqttClient.cpp
    src/Northbound/src/MqttPublisher.cpp
    src/Northbound/src/MqttProbeHub.cpp
)
target_link_libraries(myprot_northbound PUBLIC myprot_core myprot_polling)

# 南向入站 — MQTT 订阅 / TCP 监听 (设备主动推送), 与轮询事务链路并列
add_library(myprot_southbound STATIC
    src/Southbound/src/MqttSubscriber.cpp
    src/Southbound/src/PayloadMapper.cpp
    src/Southbound/src/SouthboundManager.cpp
    src/Southbound/src/TagValueSink.cpp
    src/Southbound/src/TcpIngestServer.cpp
)
target_link_libraries(myprot_southbound PUBLIC
    myprot_core myprot_transport myprot_northbound myprot_polling)

# ── 生产入口 ──
add_executable(MyProt.App
    src/App/main.cpp
    src/App/AppSignals.cpp
    src/App/CrashDiagnostics.cpp
    src/App/DataApi.cpp
    src/App/RuntimeGlue.cpp
    src/App/SimApi.cpp
    src/App/ValidateApi.cpp
    src/App/PreviewApi.cpp
    src/App/ValidateCli.cpp
    src/App/MqttProbeApi.cpp
)
target_link_libraries(MyProt.App PRIVATE
    myprot_core myprot_engine myprot_transport myprot_gateway
    myprot_polling myprot_service myprot_simulation myprot_webapi myprot_northbound
    myprot_southbound
)
if(UNIX AND NOT APPLE)
    # Linux backtrace_symbols 需要导出符号表才能显示函数名
    target_link_options(MyProt.App PRIVATE -rdynamic)
endif()

# ── 测试 ──
if(MYPROT_BUILD_TESTS)
    enable_testing()

    add_library(myprot_test_main STATIC tests/MiniTestMain.cpp)
    target_include_directories(myprot_test_main PUBLIC ${MYPROT_ROOT}/tests)
    target_link_libraries(myprot_test_main PUBLIC myprot_core)

    add_executable(MyProt.Core.Tests
        tests/Core.Tests/ByteViewTests.cpp
        tests/Core.Tests/ExpectedTests.cpp
        tests/Core.Tests/LengthFieldMathTests.cpp
    )
    target_link_libraries(MyProt.Core.Tests PRIVATE myprot_test_main myprot_core)

    add_executable(MyProt.Engine.Tests
        tests/Engine.Tests/MiniExpressionTests.cpp
        tests/Engine.Tests/RequestBuilderTests.cpp
        tests/Engine.Tests/ResponseParserTests.cpp
    )
    target_link_libraries(MyProt.Engine.Tests PRIVATE myprot_test_main myprot_engine)

    add_executable(MyProt.Service.Tests
        tests/Service.Tests/SchemaDefaultsTests.cpp
        tests/Service.Tests/ValidationIssueTests.cpp
        tests/Service.Tests/FramingSpecTests.cpp
        tests/Service.Tests/ProtoTemplateTests.cpp
        tests/Service.Tests/DerivedExprTests.cpp
        tests/Service.Tests/ConfigCommentTests.cpp
        tests/Service.Tests/TrialPreviewTests.cpp
        tests/Service.Tests/ResponsePreviewTests.cpp
    )
    target_link_libraries(MyProt.Service.Tests PRIVATE myprot_test_main myprot_service)

    add_executable(MyProt.Transport.Tests
        tests/Transport.Tests/FrameParserTests.cpp
        tests/Transport.Tests/TcpChannelTests.cpp
        tests/Transport.Tests/UdpChannelTests.cpp
        tests/Transport.Tests/StreamFrameSpecTests.cpp
    )
    target_link_libraries(MyProt.Transport.Tests PRIVATE
        myprot_test_main myprot_transport myprot_engine)

    add_executable(MyProt.Northbound.Tests
        tests/Northbound.Tests/MqttCodecTests.cpp
        tests/Northbound.Tests/MqttSessionTests.cpp
        tests/Northbound.Tests/MqttLoopbackTests.cpp
        tests/Northbound.Tests/MqttProbeTests.cpp
    )
    target_link_libraries(MyProt.Northbound.Tests PRIVATE
        myprot_test_main myprot_northbound)

    add_executable(MyProt.Southbound.Tests
        tests/Southbound.Tests/IngestLoopbackTests.cpp
        tests/Southbound.Tests/PayloadMapperTests.cpp
        tests/Southbound.Tests/TagValueSinkTests.cpp
        tests/Southbound.Tests/LatestValueStoreTests.cpp
    )
    target_link_libraries(MyProt.Southbound.Tests PRIVATE
        myprot_test_main myprot_southbound myprot_northbound myprot_polling)

    add_executable(MyProt.E2E
        src/Tests/E2EMain.cpp
        src/App/AppSignals.cpp
        src/App/CrashDiagnostics.cpp
        src/App/DataApi.cpp
        src/App/RuntimeGlue.cpp
        src/App/SimApi.cpp
        src/App/ValidateApi.cpp
        src/App/PreviewApi.cpp
    )
    target_link_libraries(MyProt.E2E PRIVATE
        myprot_core myprot_engine myprot_transport myprot_gateway
        myprot_polling myprot_service myprot_simulation myprot_webapi myprot_northbound
        myprot_southbound
    )
    if(UNIX AND NOT APPLE)
        target_link_options(MyProt.E2E PRIVATE -rdynamic)
    endif()

    add_test(NAME Core.Tests     COMMAND MyProt.Core.Tests)
    add_test(NAME Engine.Tests   COMMAND MyProt.Engine.Tests)
    add_test(NAME Service.Tests  COMMAND MyProt.Service.Tests)
    add_test(NAME Transport.Tests COMMAND MyProt.Transport.Tests)
    add_test(NAME Northbound.Tests COMMAND MyProt.Northbound.Tests)
    add_test(NAME Southbound.Tests COMMAND MyProt.Southbound.Tests)
    add_test(NAME E2E             COMMAND MyProt.E2E)
    # E2E 以相对路径读写 configs*_test, 与 VS 的 $(ProjectDir) 工作目录保持一致
    set_tests_properties(E2E PROPERTIES WORKING_DIRECTORY ${MYPROT_ROOT})
endif()

# ── 输出目录 (与 VS 的 bin/<Config> 布局对齐) ──
# 交叉编译追加目标架构子目录, 避免与主机产物互相覆盖
if(CMAKE_CROSSCOMPILING)
    set(MYPROT_BIN_DIR ${MYPROT_ROOT}/bin/$<CONFIG>/${CMAKE_SYSTEM_PROCESSOR})
else()
    set(MYPROT_BIN_DIR ${MYPROT_ROOT}/bin/$<CONFIG>)
endif()
set_target_properties(MyProt.App PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${MYPROT_BIN_DIR})
if(MYPROT_BUILD_TESTS)
    set_target_properties(MyProt.Core.Tests MyProt.Engine.Tests MyProt.Service.Tests
                          MyProt.Transport.Tests MyProt.Northbound.Tests MyProt.Southbound.Tests
                          MyProt.E2E PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${MYPROT_BIN_DIR})
endif()

# ── 安装 / 打包布局 ──
# 安装后目录 (默认相对路径可直接工作):
#   <prefix>/bin/MyProt.App
#   <prefix>/configs/protocols/*.json + tags.json
#   <prefix>/webui/dist/*            (存在时一并安装, 静态前端)
# 从 <prefix> 运行: ./bin/MyProt.App    (默认 --config configs, webRoot=webui/dist)
install(TARGETS MyProt.App RUNTIME DESTINATION bin)
install(DIRECTORY ${MYPROT_ROOT}/configs/
        DESTINATION configs
        PATTERN "*.bak.*" EXCLUDE)
if(EXISTS ${MYPROT_ROOT}/webui/dist)
    install(DIRECTORY ${MYPROT_ROOT}/webui/dist/
            DESTINATION webui/dist)
endif()

# CPack: cpack -G TGZ → <build>/package/MyProt-<version>-Linux-<arch>.tar.gz
set(CPACK_PACKAGE_NAME "MyProt")
set(CPACK_PACKAGE_VENDOR "MyProt")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "MyProt V2 protocol gateway")
set(CPACK_PACKAGE_VERSION "${MYPROT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "MyProt-${MYPROT_VERSION}-Linux-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_PACKAGE_DIRECTORY ${CMAKE_BINARY_DIR}/package)
set(CPACK_GENERATOR "TGZ")
include(CPack)

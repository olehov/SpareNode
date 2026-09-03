cmake_minimum_required(VERSION 3.25)

set(SPARENODE_VERSION "0.1.0" CACHE STRING "SpareNode semantic version")
if(NOT SPARENODE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "Invalid SpareNode version '${SPARENODE_VERSION}'. "
        "Expected MAJOR.MINOR.PATCH."
    )
endif()

project(
    SpareNode
    VERSION "${SPARENODE_VERSION}"
    DESCRIPTION "A lightweight cross-platform file server"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(SpareNodeBuildSettings)
include(SpareNodeCompilerWarnings)
include(SpareNodeCppcheck)
include(SpareNodeDocumentation)
include(SpareNodeStaticAnalysis)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/SpareNodeVersion.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/sparenode/version.hpp"
    @ONLY
)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/SpareNodeVersion.txt.in"
    "${PROJECT_BINARY_DIR}/VERSION"
    @ONLY
)

add_library(sparenode_core STATIC
    src/application/command_line.cpp
    src/application/running_application.cpp
    src/configuration/config_lexer.cpp
    src/configuration/config_loader.cpp
    src/configuration/config_parser.cpp
    src/configuration/config_validator.cpp
    src/configuration/runtime_config_mapper.cpp
    src/configuration/shared_root.cpp
    src/filesystem/detail/path_request_decoder.cpp
    src/filesystem/safe_path.cpp
    src/http/http_request.cpp
    src/http/http_request_parser.cpp
    src/http/detail/host_authority.cpp
    src/http/http_router.cpp
    src/http/http_response.cpp
    src/http/http_response_writer.cpp
    src/logging/console_log_sink.cpp
    src/logging/logger.cpp
    src/logging/log_severity.cpp
    src/logging/network_logging.cpp
    src/network/connection_dispatcher.cpp
    src/network/connection_server.cpp
    src/network/tcp_connection.cpp
    src/network/tcp_listener.cpp
    src/network/detail/accept_wait.cpp
    src/network/detail/connection_io.cpp
    src/network/detail/native_socket.cpp
    src/network/detail/socket_address.cpp
    src/network/detail/socket_poller.cpp
    src/network/detail/socket_wait.cpp
)

target_compile_features(sparenode_core PUBLIC
    cxx_std_23
)

target_include_directories(sparenode_core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(sparenode_core PRIVATE
    Threads::Threads
)

if(WIN32)
    target_link_libraries(sparenode_core PRIVATE ws2_32)
endif()

sparenode_enable_compiler_warnings(sparenode_core)
sparenode_enable_cppcheck(sparenode_core)
sparenode_enable_static_analysis(sparenode_core)

add_executable(sparenode
    apps/sparenode/main.cpp
)

target_link_libraries(sparenode PRIVATE
    sparenode_core
)

target_include_directories(sparenode PRIVATE
    "${PROJECT_BINARY_DIR}/generated"
)

sparenode_enable_compiler_warnings(sparenode)
sparenode_enable_cppcheck(sparenode)
sparenode_enable_static_analysis(sparenode)

sparenode_enable_documentation()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)

install(
    TARGETS sparenode
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_BINARY_DIR}/VERSION"
    DESTINATION .
)

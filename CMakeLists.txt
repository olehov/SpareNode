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
    src/network/accept_wait.cpp
    src/network/connection_io.cpp
    src/network/native_socket.cpp
    src/network/socket_poller.cpp
    src/network/socket_wait.cpp
    src/network/socket_address.cpp
    src/network/tcp_connection.cpp
    src/network/tcp_listener.cpp
)

target_compile_features(sparenode_core PUBLIC
    cxx_std_23
)

target_include_directories(sparenode_core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
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

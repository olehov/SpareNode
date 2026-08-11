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

add_executable(sparenode
    apps/sparenode/main.cpp
)

target_include_directories(sparenode PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_BINARY_DIR}/generated"
)

sparenode_enable_compiler_warnings(sparenode)
sparenode_enable_static_analysis(sparenode)

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

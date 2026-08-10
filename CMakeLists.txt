cmake_minimum_required(VERSION 3.25)

project(
    SpareNode
    VERSION 0.1.0
    DESCRIPTION "A lightweight cross-platform file server"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(SpareNodeBuildSettings)
include(SpareNodeCompilerWarnings)

add_executable(sparenode
    apps/sparenode/main.cpp
)

target_include_directories(sparenode PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

sparenode_enable_compiler_warnings(sparenode)

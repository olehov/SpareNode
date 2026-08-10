cmake_minimum_required(VERSION 3.25)

project(
    SpareNode
    VERSION 0.1.0
    DESCRIPTION "A lightweight cross-platform file server"
    LANGUAGES CXX
)

add_executable(sparenode
    apps/sparenode/main.cpp
)

target_compile_features(sparenode PRIVATE cxx_std_23)

target_include_directories(sparenode PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
)

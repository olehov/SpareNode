include_guard(GLOBAL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SPARENODE_SUPPORTED_COMPILERS GNU Clang MSVC)
if(NOT CMAKE_CXX_COMPILER_ID IN_LIST SPARENODE_SUPPORTED_COMPILERS)
    message(FATAL_ERROR
        "Unsupported C++ compiler '${CMAKE_CXX_COMPILER_ID}'. "
        "SpareNode supports MSVC, GCC, and Clang."
    )
endif()

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR
        "Unsupported platform '${CMAKE_SYSTEM_NAME}'. "
        "SpareNode currently supports Windows and Linux."
    )
endif()

get_property(SPARENODE_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT SPARENODE_IS_MULTI_CONFIG)
    set(SPARENODE_BUILD_TYPES Debug Release RelWithDebInfo MinSizeRel)

    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    endif()

    if(NOT CMAKE_BUILD_TYPE IN_LIST SPARENODE_BUILD_TYPES)
        message(FATAL_ERROR
            "Unsupported build type '${CMAKE_BUILD_TYPE}'. "
            "Choose one of: ${SPARENODE_BUILD_TYPES}."
        )
    endif()

    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS ${SPARENODE_BUILD_TYPES})
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/$<CONFIG>")

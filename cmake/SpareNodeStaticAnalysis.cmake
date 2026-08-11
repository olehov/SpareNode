include_guard(GLOBAL)

option(
    SPARENODE_ENABLE_CLANG_TIDY
    "Run clang-tidy while compiling first-party SpareNode targets"
    OFF
)

set(
    SPARENODE_CLANG_TIDY_EXECUTABLE
    ""
    CACHE STRING
    "Optional clang-tidy executable name or absolute path"
)

function(sparenode_enable_static_analysis target_name)
    if(NOT SPARENODE_ENABLE_CLANG_TIDY)
        return()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot configure static analysis for unknown target '${target_name}'."
        )
    endif()

    if(SPARENODE_CLANG_TIDY_EXECUTABLE)
        if(IS_ABSOLUTE "${SPARENODE_CLANG_TIDY_EXECUTABLE}")
            if(NOT EXISTS "${SPARENODE_CLANG_TIDY_EXECUTABLE}")
                message(FATAL_ERROR
                    "clang-tidy was not found at '${SPARENODE_CLANG_TIDY_EXECUTABLE}'."
                )
            endif()

            set(SPARENODE_CLANG_TIDY "${SPARENODE_CLANG_TIDY_EXECUTABLE}")
        else()
            find_program(
                SPARENODE_CLANG_TIDY
                NAMES "${SPARENODE_CLANG_TIDY_EXECUTABLE}"
                REQUIRED
            )
        endif()
    else()
        find_program(
            SPARENODE_CLANG_TIDY
            NAMES clang-tidy-18 clang-tidy
            REQUIRED
        )
    endif()

    set_property(
        TARGET "${target_name}"
        PROPERTY CXX_CLANG_TIDY
        "${SPARENODE_CLANG_TIDY};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
endfunction()

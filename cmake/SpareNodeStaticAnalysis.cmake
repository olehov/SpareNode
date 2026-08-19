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

if(SPARENODE_ENABLE_CLANG_TIDY)
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

    execute_process(
        COMMAND "${SPARENODE_CLANG_TIDY}" --version
        RESULT_VARIABLE SPARENODE_CLANG_TIDY_VERSION_RESULT
        OUTPUT_VARIABLE SPARENODE_CLANG_TIDY_VERSION_OUTPUT
        ERROR_VARIABLE SPARENODE_CLANG_TIDY_VERSION_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT SPARENODE_CLANG_TIDY_VERSION_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to query the Clang-Tidy version.")
    endif()

    string(
        REGEX MATCH
        "[0-9]+\\.[0-9]+\\.[0-9]+"
        SPARENODE_CLANG_TIDY_VERSION
        "${SPARENODE_CLANG_TIDY_VERSION_OUTPUT} ${SPARENODE_CLANG_TIDY_VERSION_ERROR}"
    )
    string(
        REGEX MATCH
        "^[0-9]+"
        SPARENODE_CLANG_TIDY_VERSION_MAJOR
        "${SPARENODE_CLANG_TIDY_VERSION}"
    )
    if(NOT SPARENODE_CLANG_TIDY_VERSION_MAJOR STREQUAL "18")
        message(FATAL_ERROR
            "Clang-Tidy 18.x is required; found "
            "'${SPARENODE_CLANG_TIDY_VERSION_OUTPUT}${SPARENODE_CLANG_TIDY_VERSION_ERROR}'."
        )
    endif()

    set(SPARENODE_CLANG_TIDY_SOURCE_REGEX "${PROJECT_SOURCE_DIR}")
    foreach(SPARENODE_REGEX_META_CHARACTER IN ITEMS
            "." "^" "$" "*" "+" "?" "(" ")" "[" "]" "{" "}" "|")
        string(
            REPLACE
            "${SPARENODE_REGEX_META_CHARACTER}"
            "\\${SPARENODE_REGEX_META_CHARACTER}"
            SPARENODE_CLANG_TIDY_SOURCE_REGEX
            "${SPARENODE_CLANG_TIDY_SOURCE_REGEX}"
        )
    endforeach()

    set(
        SPARENODE_CLANG_TIDY_HEADER_FILTER
        "^${SPARENODE_CLANG_TIDY_SOURCE_REGEX}[\\\\/](apps|include|src|tests)[\\\\/].*"
    )
    set(
        SPARENODE_CLANG_TIDY_COMMAND
        "${SPARENODE_CLANG_TIDY}"
        "--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
        "--header-filter=${SPARENODE_CLANG_TIDY_HEADER_FILTER}"
    )
endif()

function(sparenode_enable_static_analysis target_name)
    if(NOT SPARENODE_ENABLE_CLANG_TIDY)
        return()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot configure static analysis for unknown target '${target_name}'."
        )
    endif()

    set_property(
        TARGET "${target_name}"
        PROPERTY CXX_CLANG_TIDY
        "${SPARENODE_CLANG_TIDY_COMMAND}"
    )
endfunction()

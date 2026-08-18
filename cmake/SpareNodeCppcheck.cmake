include_guard(GLOBAL)

option(
    SPARENODE_ENABLE_CPPCHECK
    "Run Cppcheck while compiling first-party SpareNode targets"
    OFF
)

set(
    SPARENODE_CPPCHECK_EXECUTABLE
    ""
    CACHE STRING
    "Optional Cppcheck executable name or absolute path"
)

if(SPARENODE_ENABLE_CPPCHECK)
    if(SPARENODE_CPPCHECK_EXECUTABLE)
        if(IS_ABSOLUTE "${SPARENODE_CPPCHECK_EXECUTABLE}")
            if(NOT EXISTS "${SPARENODE_CPPCHECK_EXECUTABLE}")
                message(FATAL_ERROR
                    "Cppcheck was not found at '${SPARENODE_CPPCHECK_EXECUTABLE}'."
                )
            endif()

            set(SPARENODE_CPPCHECK "${SPARENODE_CPPCHECK_EXECUTABLE}")
        else()
            find_program(
                SPARENODE_CPPCHECK
                NAMES "${SPARENODE_CPPCHECK_EXECUTABLE}"
                REQUIRED
            )
        endif()
    else()
        find_program(SPARENODE_CPPCHECK NAMES cppcheck REQUIRED)
    endif()

    execute_process(
        COMMAND "${SPARENODE_CPPCHECK}" --version
        RESULT_VARIABLE SPARENODE_CPPCHECK_VERSION_RESULT
        OUTPUT_VARIABLE SPARENODE_CPPCHECK_VERSION_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT SPARENODE_CPPCHECK_VERSION_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to query the Cppcheck version.")
    endif()

    string(
        REGEX MATCH
        "[0-9]+\\.[0-9]+\\.[0-9]+"
        SPARENODE_CPPCHECK_VERSION
        "${SPARENODE_CPPCHECK_VERSION_OUTPUT}"
    )
    if(NOT SPARENODE_CPPCHECK_VERSION OR SPARENODE_CPPCHECK_VERSION VERSION_LESS 2.21.0)
        message(FATAL_ERROR
            "Cppcheck 2.21.0 or newer is required; found '${SPARENODE_CPPCHECK_VERSION_OUTPUT}'."
        )
    endif()

    set(
        SPARENODE_CPPCHECK_COMMAND
        "${SPARENODE_CPPCHECK}"
        "--enable=warning,performance,portability,style"
        "--error-exitcode=1"
        "--inline-suppr"
        "--std=c++23"
        "--platform=native"
        "--suppress=missingIncludeSystem"
        "--quiet"
        # Cppcheck does not parse platform system headers. The real compiler
        # still verifies that MSG_NOSIGNAL exists before using it.
        "-DMSG_NOSIGNAL=0"
    )
endif()

function(sparenode_enable_cppcheck target_name)
    if(NOT SPARENODE_ENABLE_CPPCHECK)
        return()
    endif()

    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot configure Cppcheck for unknown target '${target_name}'."
        )
    endif()

    set_property(
        TARGET "${target_name}"
        PROPERTY CXX_CPPCHECK
        "${SPARENODE_CPPCHECK_COMMAND}"
    )
endfunction()

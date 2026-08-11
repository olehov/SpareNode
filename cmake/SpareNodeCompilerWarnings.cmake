include_guard(GLOBAL)

function(sparenode_enable_compiler_warnings target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR
            "Cannot configure warnings for unknown target '${target_name}'."
        )
    endif()

    if(MSVC)
        target_compile_options("${target_name}" PRIVATE /W4 /permissive-)
    else()
        target_compile_options("${target_name}" PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

include_guard(GLOBAL)

# Register one first-party target and the first-party targets it may depend on directly.
function(sparenode_register_architecture target)
    cmake_parse_arguments(ARG "" "" "DEPENDS" ${ARGN})
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Cannot register unknown architecture target '${target}'.")
    endif()

    get_property(registered GLOBAL PROPERTY SPARENODE_ARCHITECTURE_TARGETS)
    if(target IN_LIST registered)
        message(FATAL_ERROR "Architecture target '${target}' was registered more than once.")
    endif()

    set_property(GLOBAL APPEND PROPERTY SPARENODE_ARCHITECTURE_TARGETS "${target}")
    set_property(GLOBAL PROPERTY "SPARENODE_ARCHITECTURE_DEPENDENCIES_${target}" "${ARG_DEPENDS}")
endfunction()

# Verify exact first-party target edges and reject cycles during CMake configuration.
function(sparenode_validate_architecture)
    get_property(registered GLOBAL PROPERTY SPARENODE_ARCHITECTURE_TARGETS)
    if(NOT registered)
        message(FATAL_ERROR "No first-party architecture targets were registered.")
    endif()

    foreach(target IN LISTS registered)
        get_target_property(target_type "${target}" TYPE)
        if(target_type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(link_libraries "${target}" INTERFACE_LINK_LIBRARIES)
        else()
            get_target_property(link_libraries "${target}" LINK_LIBRARIES)
        endif()
        if(link_libraries STREQUAL "link_libraries-NOTFOUND")
            set(link_libraries)
        endif()

        set(actual_dependencies)
        foreach(dependency IN LISTS link_libraries)
            if(dependency MATCHES "^sparenode_[A-Za-z0-9_]+$")
                list(APPEND actual_dependencies "${dependency}")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES actual_dependencies)
        list(SORT actual_dependencies)

        get_property(expected_dependencies GLOBAL
            PROPERTY "SPARENODE_ARCHITECTURE_DEPENDENCIES_${target}"
        )
        list(REMOVE_DUPLICATES expected_dependencies)
        list(SORT expected_dependencies)
        if(NOT "${actual_dependencies}" STREQUAL "${expected_dependencies}")
            message(FATAL_ERROR
                "Unexpected first-party dependencies for ${target}. "
                "Expected '${expected_dependencies}', found '${actual_dependencies}'."
            )
        endif()
    endforeach()

    set(remaining ${registered})
    while(remaining)
        set(next_remaining)
        set(removed_target FALSE)
        foreach(target IN LISTS remaining)
            get_property(dependencies GLOBAL
                PROPERTY "SPARENODE_ARCHITECTURE_DEPENDENCIES_${target}"
            )
            set(has_pending_dependency FALSE)
            foreach(dependency IN LISTS dependencies)
                if(dependency IN_LIST remaining)
                    set(has_pending_dependency TRUE)
                    break()
                endif()
            endforeach()

            if(has_pending_dependency)
                list(APPEND next_remaining "${target}")
            else()
                set(removed_target TRUE)
            endif()
        endforeach()

        if(NOT removed_target)
            message(FATAL_ERROR
                "First-party target dependency cycle detected among: ${remaining}"
            )
        endif()
        set(remaining ${next_remaining})
    endwhile()
endfunction()

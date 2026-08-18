option(SPARENODE_BUILD_DOCUMENTATION "Generate the SpareNode Doxygen reference" OFF)

function(sparenode_enable_documentation)
    if(NOT SPARENODE_BUILD_DOCUMENTATION)
        return()
    endif()

    find_package(Doxygen REQUIRED)

    set(SPARENODE_DOXYGEN_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/doxygen")
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/Doxyfile.in"
        "${PROJECT_BINARY_DIR}/Doxyfile"
        @ONLY
    )

    add_custom_target(sparenode_docs ALL
        COMMAND "${DOXYGEN_EXECUTABLE}" "${PROJECT_BINARY_DIR}/Doxyfile"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Generating SpareNode API documentation"
        VERBATIM
    )
endfunction()

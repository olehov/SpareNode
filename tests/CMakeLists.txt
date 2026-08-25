include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.3
    GIT_SHALLOW TRUE
    SYSTEM
)

FetchContent_MakeAvailable(Catch2)

list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(sparenode_tests
    configuration/application_config_test.cpp
    configuration/config_lexer_test.cpp
    configuration/environment_file_test.cpp
    configuration/shared_root_test.cpp
    filesystem/safe_path_test.cpp
    logging/logger_test.cpp
    network/accept_wait_test.cpp
    network/connection_dispatcher_test.cpp
    network/connection_server_test.cpp
    network/connection_io_test.cpp
    network/socket_wait_test.cpp
    network/tcp_connection_test.cpp
    network/tcp_listener_test.cpp
    smoke_test.cpp
)

target_link_libraries(sparenode_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_core
    Threads::Threads
)

if(WIN32)
    # The integration tests call Winsock directly to create their client socket.
    target_link_libraries(sparenode_tests PRIVATE ws2_32)
endif()

target_include_directories(sparenode_tests PRIVATE
    "${PROJECT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_tests)
sparenode_enable_cppcheck(sparenode_tests)
sparenode_enable_static_analysis(sparenode_tests)

catch_discover_tests(sparenode_tests)

option(
    SPARENODE_ENABLE_FAILURE_PROBE
    "Register a deliberately failing test to verify CTest failure propagation"
    OFF
)
mark_as_advanced(SPARENODE_ENABLE_FAILURE_PROBE)

if(SPARENODE_ENABLE_FAILURE_PROBE)
    add_test(
        NAME sparenode_failure_probe
        COMMAND "${CMAKE_COMMAND}" -E false
    )
endif()

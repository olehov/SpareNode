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

add_executable(sparenode_command_line_tests
    application/command_line_test.cpp
)

target_link_libraries(sparenode_command_line_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_command_line
)

target_include_directories(sparenode_command_line_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_command_line_tests)
sparenode_enable_cppcheck(sparenode_command_line_tests)
sparenode_enable_static_analysis(sparenode_command_line_tests)

catch_discover_tests(sparenode_command_line_tests PROPERTIES LABELS "unit")

add_executable(sparenode_application_tests
    application/running_application_test.cpp
)

target_link_libraries(sparenode_application_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_application
    Threads::Threads
)

option(SPARENODE_REQUIRE_SYMLINK_TESTS "Fail instead of skipping when symlink privilege is missing" OFF)
if(WIN32)
    # The application tests call Winsock directly to create their client socket.
    target_link_libraries(sparenode_application_tests PRIVATE ws2_32)
endif()

target_include_directories(sparenode_application_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_application_tests)
sparenode_enable_cppcheck(sparenode_application_tests)
sparenode_enable_static_analysis(sparenode_application_tests)

catch_discover_tests(sparenode_application_tests PROPERTIES LABELS "unit")

add_executable(sparenode_smoke_tests
    smoke_test.cpp
)

target_link_libraries(sparenode_smoke_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_foundation
)

target_include_directories(sparenode_smoke_tests PRIVATE
    "${PROJECT_BINARY_DIR}/generated"
)

sparenode_enable_compiler_warnings(sparenode_smoke_tests)
sparenode_enable_cppcheck(sparenode_smoke_tests)
sparenode_enable_static_analysis(sparenode_smoke_tests)

catch_discover_tests(sparenode_smoke_tests PROPERTIES LABELS "smoke")

add_executable(sparenode_authentication_core_tests
    authentication/authentication_service_test.cpp
)

target_link_libraries(sparenode_authentication_core_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_authentication_core
)

target_include_directories(sparenode_authentication_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_authentication_core_tests)
sparenode_enable_cppcheck(sparenode_authentication_core_tests)
sparenode_enable_static_analysis(sparenode_authentication_core_tests)

catch_discover_tests(sparenode_authentication_core_tests)

add_executable(sparenode_authentication_sodium_tests
    authentication/password_verifier_test.cpp
)

target_link_libraries(sparenode_authentication_sodium_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_authentication_sodium
)

target_include_directories(sparenode_authentication_sodium_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_authentication_sodium_tests)
sparenode_enable_cppcheck(sparenode_authentication_sodium_tests)
sparenode_enable_static_analysis(sparenode_authentication_sodium_tests)

catch_discover_tests(sparenode_authentication_sodium_tests)

add_executable(sparenode_authentication_file_tests
    authentication/credential_file_provisioner_test.cpp
    authentication/file_credential_provider_test.cpp
)

target_link_libraries(sparenode_authentication_file_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_authentication_file
    sparenode_authentication_sodium
)

target_include_directories(sparenode_authentication_file_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(SPARENODE_REQUIRE_SYMLINK_TESTS)
    target_compile_definitions(sparenode_authentication_file_tests PRIVATE
        SPARENODE_REQUIRE_SYMLINK_TESTS
    )
endif()

sparenode_enable_compiler_warnings(sparenode_authentication_file_tests)
sparenode_enable_cppcheck(sparenode_authentication_file_tests)
sparenode_enable_static_analysis(sparenode_authentication_file_tests)

catch_discover_tests(sparenode_authentication_file_tests)

add_executable(sparenode_session_core_tests
    session/session_store_test.cpp
)

target_link_libraries(sparenode_session_core_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_session_core
)

target_include_directories(sparenode_session_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_session_core_tests)
sparenode_enable_cppcheck(sparenode_session_core_tests)
sparenode_enable_static_analysis(sparenode_session_core_tests)

catch_discover_tests(sparenode_session_core_tests)

add_executable(sparenode_session_application_tests
    application/session_authentication_service_test.cpp
)

target_link_libraries(sparenode_session_application_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_session_application
)

target_include_directories(sparenode_session_application_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_session_application_tests)
sparenode_enable_cppcheck(sparenode_session_application_tests)
sparenode_enable_static_analysis(sparenode_session_application_tests)

catch_discover_tests(sparenode_session_application_tests)

add_executable(sparenode_session_sodium_tests
    session/sodium_session_credential_codec_test.cpp
)

target_link_libraries(sparenode_session_sodium_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_session_sodium
)

sparenode_enable_compiler_warnings(sparenode_session_sodium_tests)
sparenode_enable_cppcheck(sparenode_session_sodium_tests)
sparenode_enable_static_analysis(sparenode_session_sodium_tests)

catch_discover_tests(sparenode_session_sodium_tests)

add_executable(sparenode_authentication_api_tests
    authentication/session_authentication_policy_test.cpp
)

target_link_libraries(sparenode_authentication_api_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_authentication_api
)

target_include_directories(sparenode_authentication_api_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_authentication_api_tests)
sparenode_enable_cppcheck(sparenode_authentication_api_tests)
sparenode_enable_static_analysis(sparenode_authentication_api_tests)

catch_discover_tests(sparenode_authentication_api_tests)

add_executable(sparenode_http_core_tests
    http/request/http_request_parser_test.cpp
    http/routing/http_router_test.cpp
)

target_link_libraries(sparenode_http_core_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_http_core
)

target_include_directories(sparenode_http_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_http_core_tests)
sparenode_enable_cppcheck(sparenode_http_core_tests)
sparenode_enable_static_analysis(sparenode_http_core_tests)

catch_discover_tests(sparenode_http_core_tests)

add_executable(sparenode_api_core_tests
    http/api/api_router_builder_test.cpp
)

target_link_libraries(sparenode_api_core_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_api_core
)

target_include_directories(sparenode_api_core_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_api_core_tests)
sparenode_enable_cppcheck(sparenode_api_core_tests)
sparenode_enable_static_analysis(sparenode_api_core_tests)

catch_discover_tests(sparenode_api_core_tests)

add_executable(sparenode_filesystem_application_tests
    application/filesystem_service_test.cpp
)

target_link_libraries(sparenode_filesystem_application_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_filesystem_application
)

target_include_directories(sparenode_filesystem_application_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_filesystem_application_tests)
sparenode_enable_cppcheck(sparenode_filesystem_application_tests)
sparenode_enable_static_analysis(sparenode_filesystem_application_tests)

catch_discover_tests(sparenode_filesystem_application_tests)

add_executable(sparenode_filesystem_api_tests
    http/api/filesystem_api_test.cpp
)

target_link_libraries(sparenode_filesystem_api_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_filesystem_api
)

target_include_directories(sparenode_filesystem_api_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_filesystem_api_tests)
sparenode_enable_cppcheck(sparenode_filesystem_api_tests)
sparenode_enable_static_analysis(sparenode_filesystem_api_tests)

catch_discover_tests(sparenode_filesystem_api_tests)

add_executable(sparenode_http_transport_tests
    http/request/http_body_decoder_test.cpp
    http/request/request_target_test.cpp
    http/response/http_response_test.cpp
    http/transport/http_connection_handler_test.cpp
    http/transport/http_transport_policy_test.cpp
    http/transport/request_body_ingestor_test.cpp
    http/transport/request_deadlines_test.cpp
)

target_link_libraries(sparenode_http_transport_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_http_transport
    sparenode_network_logging
    Threads::Threads
)

target_include_directories(sparenode_http_transport_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(WIN32)
    target_link_libraries(sparenode_http_transport_tests PRIVATE ws2_32)
endif()

sparenode_enable_compiler_warnings(sparenode_http_transport_tests)
sparenode_enable_cppcheck(sparenode_http_transport_tests)
sparenode_enable_static_analysis(sparenode_http_transport_tests)

catch_discover_tests(sparenode_http_transport_tests)

add_executable(sparenode_filesystem_transport_integration_tests
    http/api/filesystem_api_transport_test.cpp
)

target_link_libraries(sparenode_filesystem_transport_integration_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_filesystem_api
    sparenode_http_transport
    Threads::Threads
)

target_include_directories(sparenode_filesystem_transport_integration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(WIN32)
    target_link_libraries(sparenode_filesystem_transport_integration_tests PRIVATE ws2_32)
endif()

sparenode_enable_compiler_warnings(sparenode_filesystem_transport_integration_tests)
sparenode_enable_cppcheck(sparenode_filesystem_transport_integration_tests)
sparenode_enable_static_analysis(sparenode_filesystem_transport_integration_tests)

catch_discover_tests(sparenode_filesystem_transport_integration_tests
    PROPERTIES LABELS "integration"
)

add_executable(sparenode_configuration_tests
    configuration/config_lexer_test.cpp
    configuration/config_loader_test.cpp
    configuration/config_parser_test.cpp
    configuration/config_validator_test.cpp
)

target_link_libraries(sparenode_configuration_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_configuration
)

target_include_directories(sparenode_configuration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_configuration_tests)
sparenode_enable_cppcheck(sparenode_configuration_tests)
sparenode_enable_static_analysis(sparenode_configuration_tests)

catch_discover_tests(sparenode_configuration_tests)

add_executable(sparenode_bootstrap_tests
    bootstrap/mime_types_loader_test.cpp
)

target_link_libraries(sparenode_bootstrap_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_bootstrap
)

target_include_directories(sparenode_bootstrap_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_bootstrap_tests)
sparenode_enable_cppcheck(sparenode_bootstrap_tests)
sparenode_enable_static_analysis(sparenode_bootstrap_tests)

catch_discover_tests(sparenode_bootstrap_tests)

add_executable(sparenode_bootstrap_integration_tests
    bootstrap/runtime_bootstrap_test.cpp
)

target_link_libraries(sparenode_bootstrap_integration_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_authentication_file
    sparenode_authentication_sodium
    sparenode_bootstrap
)

target_include_directories(sparenode_bootstrap_integration_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_bootstrap_integration_tests)
sparenode_enable_cppcheck(sparenode_bootstrap_integration_tests)
sparenode_enable_static_analysis(sparenode_bootstrap_integration_tests)

catch_discover_tests(sparenode_bootstrap_integration_tests PROPERTIES LABELS "integration")

add_executable(sparenode_foundation_tests
    foundation/result_test.cpp
)

target_link_libraries(sparenode_foundation_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_foundation
)

sparenode_enable_compiler_warnings(sparenode_foundation_tests)
sparenode_enable_cppcheck(sparenode_foundation_tests)
sparenode_enable_static_analysis(sparenode_foundation_tests)

catch_discover_tests(sparenode_foundation_tests)

add_executable(sparenode_storage_tests
    storage/temporary_file_test.cpp
    storage/temporary_file_writer_test.cpp
)

target_link_libraries(sparenode_storage_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_storage
    Threads::Threads
)

sparenode_enable_compiler_warnings(sparenode_storage_tests)
sparenode_enable_cppcheck(sparenode_storage_tests)
sparenode_enable_static_analysis(sparenode_storage_tests)

catch_discover_tests(sparenode_storage_tests)

add_executable(sparenode_filesystem_tests
    filesystem/directory_creator_test.cpp
    filesystem/directory_listing_test.cpp
    filesystem/file_read_stream_test.cpp
    filesystem/filesystem_entry_remover_test.cpp
    filesystem/filesystem_security_test.cpp
    filesystem/safe_path_test.cpp
    filesystem/safe_path_symlink_test.cpp
    filesystem/shared_root_test.cpp
    filesystem/upload_finalizer_test.cpp
)

if(WIN32)
    target_sources(sparenode_filesystem_tests PRIVATE
        filesystem/windows_reparse_point_test.cpp
    )
endif()

if(SPARENODE_REQUIRE_SYMLINK_TESTS)
    target_compile_definitions(sparenode_filesystem_tests PRIVATE SPARENODE_REQUIRE_SYMLINK_TESTS)
endif()

target_link_libraries(sparenode_filesystem_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_filesystem
    Threads::Threads
)

target_include_directories(sparenode_filesystem_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

sparenode_enable_compiler_warnings(sparenode_filesystem_tests)
sparenode_enable_cppcheck(sparenode_filesystem_tests)
sparenode_enable_static_analysis(sparenode_filesystem_tests)

catch_discover_tests(sparenode_filesystem_tests)

add_executable(sparenode_logging_tests
    logging/logger_test.cpp
)

target_link_libraries(sparenode_logging_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_logging
    Threads::Threads
)

sparenode_enable_compiler_warnings(sparenode_logging_tests)
sparenode_enable_cppcheck(sparenode_logging_tests)
sparenode_enable_static_analysis(sparenode_logging_tests)

catch_discover_tests(sparenode_logging_tests)

add_executable(sparenode_network_tests
    network/accept_wait_test.cpp
    network/connection_dispatcher_test.cpp
    network/connection_server_test.cpp
    network/connection_io_test.cpp
    network/socket_wait_test.cpp
    network/tcp_connection_test.cpp
    network/tcp_listener_test.cpp
)

target_link_libraries(sparenode_network_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_network
    Threads::Threads
)

target_include_directories(sparenode_network_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if(WIN32)
    # The network tests call Winsock directly to create their client socket.
    target_link_libraries(sparenode_network_tests PRIVATE ws2_32)
endif()

sparenode_enable_compiler_warnings(sparenode_network_tests)
sparenode_enable_cppcheck(sparenode_network_tests)
sparenode_enable_static_analysis(sparenode_network_tests)

catch_discover_tests(sparenode_network_tests)

add_executable(sparenode_network_logging_tests
    adapters/network_logging_test.cpp
)

target_link_libraries(sparenode_network_logging_tests PRIVATE
    Catch2::Catch2WithMain
    sparenode_network_logging
)

sparenode_enable_compiler_warnings(sparenode_network_logging_tests)
sparenode_enable_cppcheck(sparenode_network_logging_tests)
sparenode_enable_static_analysis(sparenode_network_logging_tests)

catch_discover_tests(sparenode_network_logging_tests)

add_test(
    NAME sparenode_filesystem_security_suite
    COMMAND $<TARGET_FILE:sparenode_filesystem_tests> "[filesystem][security]" --reporter console
)
set_tests_properties(sparenode_filesystem_security_suite PROPERTIES
    LABELS "filesystem;security"
)

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

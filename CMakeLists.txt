cmake_minimum_required(VERSION 3.25)

set(SPARENODE_VERSION "0.1.0" CACHE STRING "SpareNode semantic version")
if(NOT SPARENODE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "Invalid SpareNode version '${SPARENODE_VERSION}'. "
        "Expected MAJOR.MINOR.PATCH."
    )
endif()

project(
    SpareNode
    VERSION "${SPARENODE_VERSION}"
    DESCRIPTION "A lightweight cross-platform file server"
    LANGUAGES CXX
)

find_package(Threads REQUIRED)

include(FetchContent)

set(SODIUM_DISABLE_TESTS ON)
set(SODIUM_MINIMAL ON)
FetchContent_Declare(
    Sodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG 9b2848dfc1b917a9410f0de9d81059b26cbfaa8d
    GIT_SUBMODULES_RECURSE TRUE
    SYSTEM
)
FetchContent_MakeAvailable(Sodium)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(SpareNodeBuildSettings)
include(SpareNodeCompilerWarnings)
include(SpareNodeCppcheck)
include(SpareNodeDocumentation)
include(SpareNodeStaticAnalysis)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/SpareNodeVersion.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/sparenode/version.hpp"
    @ONLY
)

configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/SpareNodeVersion.txt.in"
    "${PROJECT_BINARY_DIR}/VERSION"
    @ONLY
)

add_library(sparenode_foundation INTERFACE)

target_compile_features(sparenode_foundation INTERFACE
    cxx_std_23
)

target_include_directories(sparenode_foundation INTERFACE
    "${PROJECT_SOURCE_DIR}/include"
)

add_library(sparenode_logging STATIC
    src/logging/console_log_sink.cpp
    src/logging/logger.cpp
    src/logging/log_severity.cpp
)

target_link_libraries(sparenode_logging PUBLIC
    sparenode_foundation
)

sparenode_enable_compiler_warnings(sparenode_logging)
sparenode_enable_cppcheck(sparenode_logging)
sparenode_enable_static_analysis(sparenode_logging)

add_library(sparenode_network STATIC
    src/network/connection_dispatcher.cpp
    src/network/connection_server.cpp
    src/network/tcp_connection.cpp
    src/network/tcp_listener.cpp
    src/network/detail/accept_wait.cpp
    src/network/detail/connection_io.cpp
    src/network/detail/native_socket.cpp
    src/network/detail/socket_address.cpp
    src/network/detail/socket_poller.cpp
    src/network/detail/socket_wait.cpp
)

target_link_libraries(sparenode_network
    PUBLIC sparenode_foundation
    PRIVATE Threads::Threads
)

if(WIN32)
    target_link_libraries(sparenode_network PRIVATE ws2_32)
endif()

sparenode_enable_compiler_warnings(sparenode_network)
sparenode_enable_cppcheck(sparenode_network)
sparenode_enable_static_analysis(sparenode_network)

add_library(sparenode_storage STATIC
    src/storage/temporary_file.cpp
    src/storage/temporary_file_writer.cpp
)

target_link_libraries(sparenode_storage
    PUBLIC sparenode_foundation
    PRIVATE Threads::Threads
)

sparenode_enable_compiler_warnings(sparenode_storage)
sparenode_enable_cppcheck(sparenode_storage)
sparenode_enable_static_analysis(sparenode_storage)

add_library(sparenode_filesystem STATIC
    src/filesystem/detail/path_request_decoder.cpp
    src/filesystem/directory_creator.cpp
    src/filesystem/directory_listing.cpp
    src/filesystem/filesystem_entry_remover.cpp
    src/filesystem/safe_path.cpp
    src/filesystem/shared_root.cpp
    src/filesystem/upload_finalizer.cpp
)

if(WIN32)
    target_sources(sparenode_filesystem PRIVATE
        src/filesystem/detail/windows_handle.cpp
        src/filesystem/detail/windows_handle_path.cpp
        src/filesystem/detail/windows_file_read_stream.cpp
        src/filesystem/detail/windows_directory_creator.cpp
        src/filesystem/detail/windows_directory_entry_reader.cpp
        src/filesystem/detail/windows_filesystem_entry_remover.cpp
        src/filesystem/detail/windows_path_inspector.cpp
        src/filesystem/detail/windows_upload_finalizer.cpp
    )
else()
    target_sources(sparenode_filesystem PRIVATE
        src/filesystem/detail/linux_file_read_stream.cpp
        src/filesystem/detail/linux_directory_creator.cpp
        src/filesystem/detail/linux_directory_entry_reader.cpp
        src/filesystem/detail/linux_filesystem_entry_remover.cpp
        src/filesystem/detail/linux_upload_finalizer.cpp
    )
endif()

target_link_libraries(sparenode_filesystem PUBLIC
    sparenode_foundation
    sparenode_storage
)

if(WIN32)
    target_link_libraries(sparenode_filesystem PRIVATE ntdll)
endif()

sparenode_enable_compiler_warnings(sparenode_filesystem)
sparenode_enable_cppcheck(sparenode_filesystem)
sparenode_enable_static_analysis(sparenode_filesystem)

add_library(sparenode_network_logging STATIC
    src/adapters/network_logging.cpp
)

target_link_libraries(sparenode_network_logging PUBLIC
    sparenode_logging
    sparenode_network
)

sparenode_enable_compiler_warnings(sparenode_network_logging)
sparenode_enable_cppcheck(sparenode_network_logging)
sparenode_enable_static_analysis(sparenode_network_logging)

add_library(sparenode_configuration STATIC
    src/configuration/config_lexer.cpp
    src/configuration/config_loader.cpp
    src/configuration/config_parser.cpp
    src/configuration/config_validator.cpp
)

target_link_libraries(sparenode_configuration PUBLIC
    sparenode_foundation
)

sparenode_enable_compiler_warnings(sparenode_configuration)
sparenode_enable_cppcheck(sparenode_configuration)
sparenode_enable_static_analysis(sparenode_configuration)

add_library(sparenode_core STATIC
    src/authentication/authentication_service.cpp
    src/authentication/credential_file_provisioner.cpp
    src/authentication/file_authentication.cpp
    src/authentication/file_credential_provider.cpp
    src/authentication/password_verifier.cpp
    src/authentication/sensitive_password.cpp
    src/application/command_line.cpp
    src/application/running_application.cpp
    src/http/api/filesystem_api.cpp
    src/http/mime_type_registry.cpp
    src/http/session/http_connection_handler.cpp
    src/http/session/http_transport_policy.cpp
    src/http/request/http_request.cpp
    src/http/request/http_request_parser.cpp
    src/http/request/http_body_decoder.cpp
    src/http/request/request_body_ingestor.cpp
    src/http/request/detail/host_authority.cpp
    src/http/request/detail/request_target.cpp
    src/http/response/detail/response_date.cpp
    src/http/session/detail/request_deadlines.cpp
    src/http/session/detail/buffered_request.cpp
    src/http/routing/http_router.cpp
    src/http/response/http_response.cpp
    src/http/response/http_response_writer.cpp
)

target_compile_features(sparenode_core PUBLIC
    cxx_std_23
)

target_include_directories(sparenode_core PUBLIC
    "${PROJECT_SOURCE_DIR}/include"
)

target_link_libraries(sparenode_core PRIVATE
    sodium
    sparenode_configuration
    Threads::Threads
)

target_link_libraries(sparenode_core PUBLIC
    sparenode_foundation
    sparenode_filesystem
    sparenode_logging
    sparenode_network
    sparenode_storage
)

if(WIN32)
    target_link_libraries(sparenode_core PRIVATE advapi32)
endif()

sparenode_enable_compiler_warnings(sparenode_core)
sparenode_enable_cppcheck(sparenode_core)
sparenode_enable_static_analysis(sparenode_core)

add_library(sparenode_bootstrap STATIC
    src/bootstrap/mime_types_loader.cpp
    src/bootstrap/runtime_bootstrap.cpp
)

target_link_libraries(sparenode_bootstrap PUBLIC
    sparenode_configuration
    sparenode_core
)

sparenode_enable_compiler_warnings(sparenode_bootstrap)
sparenode_enable_cppcheck(sparenode_bootstrap)
sparenode_enable_static_analysis(sparenode_bootstrap)

add_executable(sparenode
    apps/sparenode/main.cpp
)

target_link_libraries(sparenode PRIVATE
    sparenode_bootstrap
    sparenode_core
    sparenode_network_logging
)

target_include_directories(sparenode PRIVATE
    "${PROJECT_BINARY_DIR}/generated"
)

sparenode_enable_compiler_warnings(sparenode)
sparenode_enable_cppcheck(sparenode)
sparenode_enable_static_analysis(sparenode)

add_executable(sparenode_credentials
    apps/sparenode-credentials/main.cpp
)

set_target_properties(sparenode_credentials PROPERTIES
    OUTPUT_NAME "sparenode-credentials"
)

target_link_libraries(sparenode_credentials PRIVATE
    sparenode_core
)

sparenode_enable_compiler_warnings(sparenode_credentials)
sparenode_enable_cppcheck(sparenode_credentials)
sparenode_enable_static_analysis(sparenode_credentials)

sparenode_enable_documentation()

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)

install(
    TARGETS sparenode sparenode_credentials
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(
    FILES
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${PROJECT_SOURCE_DIR}/README.md"
        "${PROJECT_BINARY_DIR}/VERSION"
    DESTINATION .
)

install(
    FILES "${PROJECT_SOURCE_DIR}/config/mime.types"
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/sparenode"
)

install(
    FILES "${sodium_SOURCE_DIR}/libsodium/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/sparenode"
    RENAME libsodium-LICENSE
)

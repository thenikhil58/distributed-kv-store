cmake_minimum_required(VERSION 3.15)
project(DistributedKVStore VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(GNUInstallDirs)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(BUILD_BENCHMARKS "Build the end-to-end benchmark client" ON)
set(KVSTORE_TSAN_GRPC_SOURCE_DIR "" CACHE PATH
    "Path to a gRPC source checkout with initialized submodules, for an instrumented TSan build")

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

if(ENABLE_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

if(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(KVSTORE_TSAN_GRPC_SOURCE_DIR)
    if(NOT ENABLE_TSAN)
        message(FATAL_ERROR "KVSTORE_TSAN_GRPC_SOURCE_DIR requires ENABLE_TSAN=ON")
    endif()

    foreach(required_path
            CMakeLists.txt
            third_party/abseil-cpp/CMakeLists.txt
            third_party/protobuf/cmake/CMakeLists.txt
            third_party/re2/CMakeLists.txt
            third_party/cares/cares/CMakeLists.txt
            third_party/zlib/CMakeLists.txt)
        if(NOT EXISTS "${KVSTORE_TSAN_GRPC_SOURCE_DIR}/${required_path}")
            message(FATAL_ERROR
                "The gRPC checkout is missing ${required_path}; initialize its required submodules")
        endif()
    endforeach()

    file(READ "${KVSTORE_TSAN_GRPC_SOURCE_DIR}/CMakeLists.txt" grpc_cmake_header LIMIT 2048)
    if(NOT grpc_cmake_header MATCHES "set\\(PACKAGE_VERSION[ \t]+\"1\\.51\\.1\"\\)")
        message(FATAL_ERROR "The instrumented TSan dependency checkout must be gRPC v1.51.1")
    endif()

    set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
    set(gRPC_ABSL_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_PROTOBUF_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_RE2_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_CARES_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_ZLIB_PROVIDER module CACHE STRING "" FORCE)
    set(gRPC_SSL_PROVIDER package CACHE STRING "" FORCE)
    add_subdirectory("${KVSTORE_TSAN_GRPC_SOURCE_DIR}"
                     "${CMAKE_BINARY_DIR}/grpc" EXCLUDE_FROM_ALL)

    if(NOT TARGET grpc++ OR NOT TARGET protobuf::libprotobuf)
        message(FATAL_ERROR "The instrumented gRPC build did not provide required C++ targets")
    endif()
    set(KVSTORE_GRPC_TARGET grpc++)
else()
    find_package(gRPC CONFIG REQUIRED)
    find_package(Protobuf REQUIRED)
    set(KVSTORE_GRPC_TARGET gRPC::grpc++)
endif()
find_package(GTest REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(server
    server.cpp
    runtime_config.cpp
    kvstore.pb.cc
    kvstore.grpc.pb.cc
)

target_include_directories(server
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(server
    PRIVATE
    ${KVSTORE_GRPC_TARGET}
    protobuf::libprotobuf
    OpenSSL::Crypto
)

add_executable(client
    client.cpp
    kvstore.pb.cc
    kvstore.grpc.pb.cc
)

target_include_directories(client
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(client
    PRIVATE
    ${KVSTORE_GRPC_TARGET}
    protobuf::libprotobuf
)

add_executable(raft_tests
    tests/test_raft_integration.cpp
    kvstore.pb.cc
    kvstore.grpc.pb.cc
)

target_include_directories(raft_tests
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(runtime_config_tests
    tests/test_runtime_config.cpp
    runtime_config.cpp
)

target_include_directories(runtime_config_tests
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(BUILD_BENCHMARKS)
    add_executable(kvstore_benchmark
        benchmarks/kvstore_benchmark.cpp
        kvstore.pb.cc
        kvstore.grpc.pb.cc
    )
    target_include_directories(kvstore_benchmark
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(kvstore_benchmark PRIVATE ${KVSTORE_GRPC_TARGET} protobuf::libprotobuf)
endif()

target_link_libraries(raft_tests
    PRIVATE
    ${KVSTORE_GRPC_TARGET}
    protobuf::libprotobuf
    GTest::gtest_main
    OpenSSL::Crypto
)

target_link_libraries(runtime_config_tests
    PRIVATE
    GTest::gtest_main
)

add_test(NAME raft_tests COMMAND raft_tests)
add_test(NAME runtime_config_tests COMMAND runtime_config_tests)
set_tests_properties(raft_tests runtime_config_tests PROPERTIES TIMEOUT 600)

enable_testing()

install(TARGETS server client
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
if(BUILD_BENCHMARKS)
    install(TARGETS kvstore_benchmark
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
install(FILES config/node.conf.example
    DESTINATION ${CMAKE_INSTALL_DATADIR}/distributed-kv-store
)
install(PROGRAMS tools/rotate_credentials.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)

set(CPACK_PACKAGE_NAME "distributed-kv-store")
set(CPACK_PACKAGE_VENDOR "thenikhil58")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Experimental Raft-based distributed key-value store")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
include(CPack)

# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.30...4.4)

project(
    async_prototypes
    DESCRIPTION "Experiments behind my WG21 work on asynchronous functions"
    LANGUAGES C CXX
)

# [CMAKE.SKIP_TESTS]
option(
    ASYNC_PROTOTYPES_BUILD_TESTS
    "Register the experiments' checks with CTest. Default: ${PROJECT_IS_TOP_LEVEL}. Values: { ON, OFF }."
    ${PROJECT_IS_TOP_LEVEL}
)

# [CMAKE.PASSIVE_PROJECTS]: the standard comes from the user or a preset, so
# check it rather than set it.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles(
    "
    #if __cplusplus < 202002L
    #error C++20 is required
    #endif
    int main() {}
    "
    ASYNC_PROTOTYPES_HAVE_CXX20
)
if(NOT ASYNC_PROTOTYPES_HAVE_CXX20)
    message(
        FATAL_ERROR
        "The experiments need C++20 or later. Configure with a preset "
        "(`cmake --list-presets`), or pass -DCMAKE_CXX_STANDARD=23."
    )
endif()

find_package(Threads REQUIRED)

include(infra/cmake/beman-xfail-compile.cmake)
include(cmake/stdexec.cmake)

if(ASYNC_PROTOTYPES_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(direct-call)
add_subdirectory(app-scope)
add_subdirectory(domains-attrs)
add_subdirectory(frames-stress)
add_subdirectory(kv-handle)
add_subdirectory(sp-stack)
add_subdirectory(kv-dest)
add_subdirectory(tony)

# SPDX-License-Identifier: MIT

# The C convention, three ways: plain, counting live frames (ASYNC_CHECK),
# and recording the async stack's high-water mark (ASYNC_STATS). The
# switches change inline functions in async_abi.h, so each program is
# compiled with the same switch as the library it links.
foreach(variant abi abi_check abi_stats)
    add_library(async_prototypes.kv_dest.${variant} STATIC)
    target_sources(async_prototypes.kv_dest.${variant} PRIVATE async_abi.c)
endforeach()
target_compile_definitions(
    async_prototypes.kv_dest.abi_check
    PRIVATE ASYNC_CHECK
)
target_compile_definitions(
    async_prototypes.kv_dest.abi_stats
    PRIVATE ASYNC_STATS
)

add_executable(async_prototypes.kv_dest.checks)
target_sources(async_prototypes.kv_dest.checks PRIVATE stores.cpp checks.cpp)
target_compile_definitions(async_prototypes.kv_dest.checks PRIVATE ASYNC_CHECK)
target_link_libraries(
    async_prototypes.kv_dest.checks
    PRIVATE async_prototypes.kv_dest.abi_check
)

add_executable(async_prototypes.kv_dest.count)
target_sources(async_prototypes.kv_dest.count PRIVATE stores.cpp bench.cpp)
target_compile_definitions(
    async_prototypes.kv_dest.count
    PRIVATE COUNT_MOVES ASYNC_STATS
)
target_link_libraries(
    async_prototypes.kv_dest.count
    PRIVATE async_prototypes.kv_dest.abi_stats
)

add_executable(async_prototypes.kv_dest.bench)
target_sources(async_prototypes.kv_dest.bench PRIVATE stores.cpp bench.cpp)
target_link_libraries(
    async_prototypes.kv_dest.bench
    PRIVATE async_prototypes.kv_dest.abi
)

# The rigs leak their stores on purpose.
add_test(NAME kv_dest.checks COMMAND async_prototypes.kv_dest.checks)
set_tests_properties(
    kv_dest.checks
    PROPERTIES PASS_REGULAR_EXPRESSION "498 runs, 0 failures"
)
add_test(NAME kv_dest.count COMMAND async_prototypes.kv_dest.count)
# A short timing run, which aborts if any variant gets a wrong answer.
add_test(NAME kv_dest.bench COMMAND async_prototypes.kv_dest.bench 1000)
set_tests_properties(
    kv_dest.checks
    kv_dest.count
    kv_dest.bench
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)

# An immovable value type through a filtering middle, one variant per
# program. The single slot (rows 1A and 1B) has to move the value, so those
# two must fail to compile on the deleted move constructor.
foreach(variant V2A V3A V4A V5A V2B V3B V4B V5B)
    set(target async_prototypes.kv_dest.pinned.${variant})
    add_executable(${target})
    target_sources(${target} PRIVATE pinned.cpp)
    target_compile_definitions(${target} PRIVATE VARIANT=${variant})
    target_link_libraries(${target} PRIVATE async_prototypes.kv_dest.abi)
    add_test(NAME kv_dest.pinned.${variant} COMMAND ${target})
endforeach()
foreach(variant V1A V1B)
    beman_add_xfail_compile_test(
        kv_dest.pinned.${variant}
        SOURCE pinned.cpp
        EXPECT "deleted function [^ ]*pinned::pinned\\(pinned&&\\)|deleted constructor of [^ ]*pinned"
        COMPILE_DEFINITIONS VARIANT=${variant}
        TARGET_PREFIX async_prototypes.xfail.
    )
endforeach()

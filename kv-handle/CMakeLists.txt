# SPDX-License-Identifier: MIT

async_prototypes_stdexec(5afe072)

# The asynchronous calling convention, and a store written against it, in C.
add_library(async_prototypes.kv_handle.abi STATIC)
target_sources(async_prototypes.kv_handle.abi PRIVATE async_abi.c)

add_library(async_prototypes.kv_handle.c_store STATIC)
target_sources(async_prototypes.kv_handle.c_store PRIVATE c_store.c)
target_link_libraries(
    async_prototypes.kv_handle.c_store
    PRIVATE async_prototypes.kv_handle.abi
)

# The README's three builds of the stores: the checks, the copy and move
# counts, and the timings.
foreach(program checks count bench)
    set(target async_prototypes.kv_handle.${program})
    add_executable(${target})
    target_link_libraries(
        ${target}
        PRIVATE async_prototypes.kv_handle.abi async_prototypes.stdexec.5afe072
    )
endforeach()
target_sources(async_prototypes.kv_handle.checks PRIVATE stores.cpp checks.cpp)
target_sources(async_prototypes.kv_handle.count PRIVATE stores.cpp bench.cpp)
target_compile_definitions(
    async_prototypes.kv_handle.count
    PRIVATE COUNT_MOVES COUNT_HEAP ASYNC_STATS
)
target_sources(async_prototypes.kv_handle.bench PRIVATE stores.cpp bench.cpp)

add_executable(async_prototypes.kv_handle.c_demo)
target_sources(async_prototypes.kv_handle.c_demo PRIVATE c_demo.cpp)
target_link_libraries(
    async_prototypes.kv_handle.c_demo
    PRIVATE
        async_prototypes.kv_handle.c_store
        async_prototypes.kv_handle.abi
        async_prototypes.stdexec.5afe072
)

# The drivers leak their stores on purpose.
add_test(NAME kv_handle.checks COMMAND async_prototypes.kv_handle.checks)
add_test(NAME kv_handle.count COMMAND async_prototypes.kv_handle.count)
# A short timing run, which aborts if any configuration gets a wrong answer.
add_test(NAME kv_handle.bench COMMAND async_prototypes.kv_handle.bench 1000)
set_tests_properties(
    kv_handle.checks
    kv_handle.count
    kv_handle.bench
    PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
)

add_test(NAME kv_handle.c_demo COMMAND async_prototypes.kv_handle.c_demo)
set_tests_properties(
    kv_handle.c_demo
    PROPERTIES
        PASS_REGULAR_EXPRESSION
            "entry  get_len\\(\"k2\"\\): value 9 \\(after suspending\\)\nentry  get_len\\(\"nope\"\\): value 0 \\(after suspending\\)\nentry  get_len\\(\"\"\\): error 22 \\(inline\\)\nsender get_len \\| then\\(n \\* 2\\): value 18\nsender get_len \\| then\\(n \\* 2\\): error 22 \\(EINVAL\\)\n"
)

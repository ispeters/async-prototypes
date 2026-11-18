# SPDX-License-Identifier: MIT

# The machine-stack convention is x86-64 System V inline assembly.
if(
    NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
    OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$"
)
    message(
        WARNING
        "sp-stack needs x86-64 Linux, not ${CMAKE_SYSTEM_NAME} on ${CMAKE_SYSTEM_PROCESSOR}. Skip: sp-stack"
    )
    return()
endif()

# AddressSanitizer and ThreadSanitizer can't follow frames that are left on
# the stack after their function returns, or a stack switch, and report
# false errors. UBSan alone is fine.
include(CheckCXXSourceCompiles)
set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
check_cxx_source_compiles(
    "
    #if defined(__has_feature)
    #if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    #error sanitizer
    #endif
    #endif
    #if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    #error sanitizer
    #endif
    int main() {}
    "
    ASYNC_PROTOTYPES_SP_STACK_NO_ASAN_OR_TSAN
)
if(NOT ASYNC_PROTOTYPES_SP_STACK_NO_ASAN_OR_TSAN)
    message(
        WARNING
        "sp-stack can't run under AddressSanitizer or ThreadSanitizer. Skip: sp-stack"
    )
    return()
endif()

# async_abi.c is kv-handle's; this directory links to it.
add_library(async_prototypes.sp_stack.abi STATIC)
target_sources(async_prototypes.sp_stack.abi PRIVATE async_abi.c)

# MS_LEAVE and the stack switches address frames through rbp, so every
# function in the convention needs a frame pointer.
function(sp_stack_program name)
    set(target async_prototypes.sp_stack.${name})
    add_executable(${target})
    target_sources(${target} PRIVATE ${ARGN} mstack.cpp)
    target_compile_options(${target} PRIVATE -fno-omit-frame-pointer)
    target_link_libraries(${target} PRIVATE async_prototypes.sp_stack.abi)
endfunction()

sp_stack_program(checks checks.cpp stores.cpp)
sp_stack_program(bench bench.cpp stores.cpp)
sp_stack_program(sizes bench.cpp stores.cpp)
target_compile_definitions(async_prototypes.sp_stack.sizes PRIVATE ASYNC_STATS)
sp_stack_program(limits limits.cpp stores.cpp)
sp_stack_program(tree tree_bench.cpp tree.cpp)
target_compile_definitions(async_prototypes.sp_stack.tree PRIVATE ASYNC_STATS)
# Without the callee-saved-register marker, which is safe only where nothing
# suspends: run it with "inline".
sp_stack_program(tree_nc tree_bench.cpp tree.cpp)
target_compile_definitions(async_prototypes.sp_stack.tree_nc PRIVATE MS_NO_CLOBBER)

add_test(NAME sp_stack.checks COMMAND async_prototypes.sp_stack.checks)
add_test(NAME sp_stack.sizes COMMAND async_prototypes.sp_stack.sizes sizes)
add_test(NAME sp_stack.tree COMMAND async_prototypes.sp_stack.tree)
add_test(NAME sp_stack.tree_nc COMMAND async_prototypes.sp_stack.tree_nc inline)
foreach(experiment depths overflow-guard overflow-arena)
    add_test(
        NAME sp_stack.limits.${experiment}
        COMMAND async_prototypes.sp_stack.limits ${experiment}
    )
endforeach()
foreach(column m c)
    add_test(
        NAME sp_stack.limits.rss.${column}
        COMMAND async_prototypes.sp_stack.limits rss ${column} 10000
    )
endforeach()
# `limits maps` isn't a test: it maps stacks until the system refuses.

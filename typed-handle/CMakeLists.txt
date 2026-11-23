# SPDX-License-Identifier: MIT

# stdexec provides the completion_signatures machinery, nothing else.
async_prototypes_stdexec(30acdb0)

# One program per mode: EAGER (0), ERASED (1), TYPED (2), VARIANT (3).
foreach(mode 0 1 2 3)
    add_executable(async_prototypes.typed_handle.checks.${mode})
    target_sources(async_prototypes.typed_handle.checks.${mode} PRIVATE checks.cpp)
    target_compile_definitions(
        async_prototypes.typed_handle.checks.${mode}
        PRIVATE TH_MODE=${mode}
    )
    target_link_libraries(
        async_prototypes.typed_handle.checks.${mode}
        PRIVATE async_prototypes.stdexec.30acdb0
    )
    add_test(
        NAME typed_handle.checks.${mode}
        COMMAND async_prototypes.typed_handle.checks.${mode}
    )

    add_executable(async_prototypes.typed_handle.bench.${mode})
    target_sources(async_prototypes.typed_handle.bench.${mode} PRIVATE bench.cpp)
    target_compile_definitions(
        async_prototypes.typed_handle.bench.${mode}
        PRIVATE TH_MODE=${mode}
    )
    target_link_libraries(
        async_prototypes.typed_handle.bench.${mode}
        PRIVATE async_prototypes.stdexec.30acdb0
    )
    # A short run of every scenario, which fails if one gets a wrong answer.
    foreach(
        scenario
        chain4 chain4_throwing let both boundary repeat
        chain4_unstoppable chain4_checked both_live both_checked
        boundary_checked
        repeat_checked
    )
        add_test(
            NAME typed_handle.bench.${mode}.${scenario}
            COMMAND async_prototypes.typed_handle.bench.${mode} ${scenario} 100
        )
    endforeach()
endforeach()

# In EAGER mode, repeat_until's again() can't deduce its own return type: the
# restart nests a call that ends in again(). The other modes can.
foreach(mode 1 2 3)
    add_executable(async_prototypes.typed_handle.deduced.${mode})
    target_sources(async_prototypes.typed_handle.deduced.${mode} PRIVATE checks.cpp)
    target_compile_definitions(
        async_prototypes.typed_handle.deduced.${mode}
        PRIVATE TH_MODE=${mode} TH_DEDUCE_AGAIN
    )
    target_link_libraries(
        async_prototypes.typed_handle.deduced.${mode}
        PRIVATE async_prototypes.stdexec.30acdb0
    )
endforeach()
beman_add_xfail_compile_test(
    typed_handle.deduced.0
    SOURCE checks.cpp
    EXPECT "before deduction of [^a ]*auto|deduced return type cannot be used before it is defined"
    COMPILE_DEFINITIONS TH_MODE=0 TH_DEDUCE_AGAIN
    LINK_LIBRARIES async_prototypes.stdexec.30acdb0
    TARGET_PREFIX async_prototypes.xfail.
)

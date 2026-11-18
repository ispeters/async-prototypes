# SPDX-License-Identifier: MIT

async_prototypes_stdexec(e72e0fa)

set(programs added_error)
# Clang 18 crashes generating code for common.hpp's start_env; 19 and 20
# don't.
if(
    CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
    AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19
)
    message(
        WARNING
        "Clang ${CMAKE_CXX_COMPILER_VERSION} crashes on domains-attrs/common.hpp. "
        "Skip: domains-attrs/lowered.cpp, domains-attrs/consequences.cpp"
    )
else()
    list(APPEND programs lowered consequences)
endif()

# Each program prints what it observed; the README records that output. The
# tests check only that the programs run.
foreach(program ${programs})
    add_executable(async_prototypes.domains_attrs.${program})
    target_sources(
        async_prototypes.domains_attrs.${program}
        PRIVATE ${program}.cpp
    )
    target_link_libraries(
        async_prototypes.domains_attrs.${program}
        PRIVATE async_prototypes.stdexec.e72e0fa
    )
    add_test(
        NAME domains_attrs.${program}
        COMMAND async_prototypes.domains_attrs.${program}
    )
endforeach()

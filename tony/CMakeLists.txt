# SPDX-License-Identifier: MIT

async_prototypes_stdexec(30acdb0)

# The C++26 column, against stdexec.
add_executable(async_prototypes.tony.tests26)
target_sources(async_prototypes.tony.tests26 PRIVATE tests26.cpp)
target_link_libraries(
    async_prototypes.tony.tests26
    PRIVATE async_prototypes.stdexec.30acdb0
)

# The C++29 column, against the stand-in in x29.hpp, and sections 10 and 11.
foreach(test tests29 erased_test lowered_test)
    add_executable(async_prototypes.tony.${test})
    target_sources(async_prototypes.tony.${test} PRIVATE ${test}.cpp)
    target_link_libraries(async_prototypes.tony.${test} PRIVATE Threads::Threads)
endforeach()

# Section 12: a leaf written in C, called from C++.
add_library(async_prototypes.tony.c_store STATIC)
target_sources(async_prototypes.tony.c_store PRIVATE c_store.c)
add_executable(async_prototypes.tony.c_test)
target_sources(async_prototypes.tony.c_test PRIVATE c_test.cpp)
target_link_libraries(
    async_prototypes.tony.c_test
    PRIVATE async_prototypes.tony.c_store
)

# Each exits with 1 on a failed check. The move counts the page reports are
# printed, not checked.
foreach(test tests26 tests29 erased_test lowered_test c_test)
    add_test(NAME tony.${test} COMMAND async_prototypes.tony.${test})
endforeach()

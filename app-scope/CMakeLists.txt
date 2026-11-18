# SPDX-License-Identifier: MIT

async_prototypes_stdexec(63a0561)

add_executable(async_prototypes.app_scope)
target_sources(async_prototypes.app_scope PRIVATE app.cpp)
target_link_libraries(
    async_prototypes.app_scope
    PRIVATE async_prototypes.stdexec.63a0561
)

# As written, the spawned work is silently dropped. The variant that wraps
# the app in `associate` (argument "a") hangs, as the README reports, so it
# isn't a test.
add_test(NAME app_scope COMMAND async_prototypes.app_scope)
set_tests_properties(
    app_scope
    PROPERTIES PASS_REGULAR_EXPRESSION "spawned work ran 0 time\\(s\\)"
)

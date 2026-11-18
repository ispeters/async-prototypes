# SPDX-License-Identifier: MIT

# The README's observations come from `objdump -dr` on the callers' object
# files, which are in this directory's part of the build tree.

add_executable(async_prototypes.direct_call)
target_sources(async_prototypes.direct_call PRIVATE caller.cpp callee.cpp)
add_test(NAME direct_call COMMAND async_prototypes.direct_call)

add_executable(async_prototypes.direct_call.virtual)
target_sources(
    async_prototypes.direct_call.virtual
    PRIVATE virtual/caller.cpp virtual/impl.cpp
)
add_test(
    NAME direct_call.virtual
    COMMAND async_prototypes.direct_call.virtual
)

# caller2.cpp has no main: it is only compiled, to be disassembled.
add_library(async_prototypes.direct_call.virtual.caller2 OBJECT)
target_sources(
    async_prototypes.direct_call.virtual.caller2
    PRIVATE virtual/caller2.cpp
)

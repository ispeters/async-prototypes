# SPDX-License-Identifier: MIT

# The tests exercise the phase-based algorithms in stdexec PR #2401's
# examples/frames, at the PR's head.
async_prototypes_stdexec(e72e0fa)
FetchContent_GetProperties(stdexec_e72e0fa SOURCE_DIR stdexec_dir)

add_library(async_prototypes.frames_stress.frames INTERFACE)
target_include_directories(
    async_prototypes.frames_stress.frames
    SYSTEM
    INTERFACE "${stdexec_dir}/examples/frames"
)
target_link_libraries(
    async_prototypes.frames_stress.frames
    INTERFACE async_prototypes.stdexec.e72e0fa
)

# Every combination in one translation unit exhausts about 8 GB, so each of
# these is split into four parts with PART. The README runs stop_test and
# assoc_test with 100 to 200 iterations; the tests use fewer, to stay quick.
foreach(test stop_test fin_test assoc_test)
    foreach(part 0 1 2 3)
        set(target async_prototypes.frames_stress.${test}.${part})
        add_executable(${target})
        target_sources(${target} PRIVATE ${test}.cpp)
        target_compile_definitions(${target} PRIVATE PART=${part})
        target_link_libraries(
            ${target}
            PRIVATE async_prototypes.frames_stress.frames
        )
        if(test STREQUAL "fin_test")
            add_test(NAME frames_stress.${test}.${part} COMMAND ${target})
        else()
            add_test(NAME frames_stress.${test}.${part} COMMAND ${target} 20)
        endif()
    endforeach()
endforeach()

# These print "bad 0" on every line when the invariant holds.
foreach(test assoc_lifetime_test assoc_throw_test)
    add_executable(async_prototypes.frames_stress.${test})
    target_sources(async_prototypes.frames_stress.${test} PRIVATE ${test}.cpp)
    target_link_libraries(
        async_prototypes.frames_stress.${test}
        PRIVATE async_prototypes.frames_stress.frames
    )
    add_test(
        NAME frames_stress.${test}
        COMMAND async_prototypes.frames_stress.${test}
    )
    set_tests_properties(
        frames_stress.${test}
        PROPERTIES
            PASS_REGULAR_EXPRESSION "bad 0"
            FAIL_REGULAR_EXPRESSION "bad [1-9]"
    )
endforeach()

# Not a test: it prints sizes and move counts.
add_executable(async_prototypes.frames_stress.assoc_meas)
target_sources(async_prototypes.frames_stress.assoc_meas PRIVATE assoc_meas.cpp)
target_link_libraries(
    async_prototypes.frames_stress.assoc_meas
    PRIVATE async_prototypes.frames_stress.frames
)

# entry_split_test: two controls that must compile and print 42, and the
# case phased::check_entry must reject at compile time.
foreach(control "split_sender;0" "stored_sender;1")
    list(GET control 0 data)
    list(GET control 1 enter_second)
    set(target async_prototypes.frames_stress.entry_split.${data}.${enter_second})
    add_executable(${target})
    target_sources(${target} PRIVATE entry_split_test.cpp)
    target_compile_definitions(
        ${target}
        PRIVATE DATA=${data} ENTER_SECOND=${enter_second}
    )
    target_link_libraries(
        ${target}
        PRIVATE async_prototypes.frames_stress.frames
    )
    add_test(
        NAME frames_stress.entry_split.${data}.${enter_second}
        COMMAND ${target}
    )
    set_tests_properties(
        frames_stress.entry_split.${data}.${enter_second}
        PROPERTIES PASS_REGULAR_EXPRESSION "^42\n$"
    )
endforeach()

beman_add_xfail_compile_test(
    frames_stress.entry_split.split_sender.1
    SOURCE entry_split_test.cpp
    EXPECT "a part the first phase connects on entry isn't kept for another phase"
    LINK_LIBRARIES async_prototypes.frames_stress.frames
    COMPILE_DEFINITIONS DATA=split_sender ENTER_SECOND=1
    TARGET_PREFIX async_prototypes.xfail.
)

# SPDX-License-Identifier: MIT

# The stdexec commits the experiments were built against. Each experiment
# keeps the commit its README reports, so several are in use at once.
# Beman's lockfile can't express that: it resolves each package through
# find_package and add_subdirectory, so it gives one version per package,
# and two copies of stdexec's own CMake project would collide. stdexec is
# header-only for these experiments, so its sources are fetched here
# without its CMake project, and each commit gets an interface target that
# provides only its include directories.
#
# To use a local checkout instead of a download, set
# FETCHCONTENT_SOURCE_DIR_<NAME>, for example
# -DFETCHCONTENT_SOURCE_DIR_STDEXEC_30ACDB0=~/src/stdexec.

include_guard(GLOBAL)
include(FetchContent)

# The SOURCE_SUBDIR doesn't exist, so FetchContent_MakeAvailable fetches the
# sources without adding stdexec's CMake project.
foreach(
    pin
    IN ITEMS
        "63a0561|https://github.com/NVIDIA/stdexec.git|63a05616d5712d8270a4cf60b68652ac3747d648"
        "5afe072|https://github.com/NVIDIA/stdexec.git|5afe072bc89627cf5e0b960ee588ee28259fca33"
        "30acdb0|https://github.com/NVIDIA/stdexec.git|30acdb01b18c05bb72f336ef306d1b9fb45ebbb0"
        # PR #2401's head. A clone of NVIDIA/stdexec doesn't contain it, so
        # it comes from the PR's branch, which must still contain it.
        "e72e0fa|https://github.com/ispeters/stdexec.git|e72e0faa1fd2711dbc9fe87ddf19ccee327271e8"
)
    string(REPLACE "|" ";" pin "${pin}")
    list(GET pin 0 short)
    list(GET pin 1 repository)
    list(GET pin 2 commit)
    FetchContent_Declare(
        stdexec_${short}
        GIT_REPOSITORY ${repository}
        GIT_TAG ${commit}
        SOURCE_SUBDIR no-cmake-project
    )
endforeach()

# async_prototypes_stdexec(<short>)
#
# Fetches stdexec at the pinned commit <short> and defines the interface
# target async_prototypes.stdexec.<short>.
function(async_prototypes_stdexec short)
    set(target async_prototypes.stdexec.${short})
    if(TARGET ${target})
        return()
    endif()
    FetchContent_MakeAvailable(stdexec_${short})
    add_library(${target} INTERFACE)
    target_include_directories(
        ${target}
        SYSTEM
        INTERFACE "${stdexec_${short}_SOURCE_DIR}/include"
    )
    target_link_libraries(${target} INTERFACE Threads::Threads)
endfunction()

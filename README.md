# Async prototypes

Experiments behind my WG21 work on asynchronous functions, sender
interfaces, and an asynchronous calling convention for C++. Each directory is
one experiment and has its own README. The README says what the experiment
shows, what was measured, and the commands it was built with.

I wrote these with Claude in a Claude project, where they lived as project
documents. They're here so they last and can be reached from outside the
project. Each was imported unchanged and then rebuilt to check that nothing
was lost in copying. A CMake build now builds and checks them all the same
way; see [Building](#building).

| Directory | What it is | stdexec |
|---|---|---|
| `direct-call/` | Calling a separately compiled async entry directly, with and without a virtual call | none |
| `app-scope/` | A programme-wide scope joined alongside the app | 63a0561 |
| `stdexec-tx/` | Patches that stop `default_domain::transform_sender` from moving each sender layer twice | applies to 34773fb |
| `frames-stress/` | Stress tests for the phase-based algorithms in stdexec PR #2401 (`examples/frames`) | PR #2401 head, e72e0fa |
| `domains-attrs/` | Domains and attributes: lowering, its consequences, and an added error | PR #2401 head, e72e0fa |
| `kv-handle/` | P4223's `kv_store` cache behind today's `function`, a handle-returning `function`, and a visible entry; C callers and callees of the ABI | 5afe072b |
| `sp-stack/` | The machine stack as the async stack, measured against `kv-handle`'s cursor | none |
| `kv-dest/` | Ten ways of telling a callee where each completion's payload goes (W-157) | none |
| `tony/` | stdexec algorithms in C++26 beside a C++29 strawman, in Tony Tables | 30acdb0 |
| `typed-handle/` | Typed operation handles (`operation_handle<R, Sig>`, with `start_t()` for starting) beside the erased one, in four modes | none |

Where a README names a file under `synthesis/` or `function-coroutines/`, that
file is a design note in the Claude project, not in this repository.

`sp-stack/async_abi.h` and `sp-stack/async_abi.c` are symbolic links to
`kv-handle`'s, which that README says to put beside its files. `kv-dest` and
`tony` carry their own variants of the ABI header.

## Building

The build follows the [Beman project](https://github.com/bemanproject)'s
conventions: presets for common compilers, Beman's toolchain files in
`infra/` (embedded with [`embd`](https://github.com/ptsouchlos/embd)), and a
project that checks the C++ standard rather than setting it.

You need CMake 3.30 or later, Ninja, git, and a C and C++ compiler with C++20.
Configuring fetches the stdexec commits in the table above with git.

With Nix, `nix develop` in the repository's root gives you CMake and Ninja.
On macOS that shell adds no compiler, so the presets use Apple's; on Linux it
adds GCC and Clang. `nix develop .#stdexec` is instead the shell I work on
stdexec in, from [my devshells](https://github.com/ispeters/devshells), with
Clang 23 and its own libc++. The `appleclang` presets use that Clang there,
because they call whatever `c++` is on the `PATH`.

```sh
cmake --list-presets                         # what this machine can use
cmake --workflow --preset appleclang-debug   # configure, build and test
```

On macOS use `appleclang-debug` or `appleclang-release`, which use the `cc`
and `c++` on your `PATH`. On Linux there are also `gcc-debug`,
`gcc-release`, `llvm-debug` and `llvm-release`. Each
builds in `build/<preset>`. Debug builds run under AddressSanitizer and
UBSan, and Release builds use `-O3`. They all use C++23 and C11.

Every experiment's checks are CTest tests. `ctest --preset <preset> -R
kv_dest` runs one experiment's. A few tests are Beman compile-fail tests,
which pass only if a source fails to compile with the expected error; `ctest
-L xfail-compile` runs just those.

Some experiments can't build everywhere. Each of these is skipped with a
warning at configure time:

- `sp-stack` needs x86-64 Linux, so it isn't built on Apple silicon. It also
  can't run under AddressSanitizer or ThreadSanitizer, so only the Release
  presets build it.
- `domains-attrs/lowered.cpp` and `consequences.cpp` crash Clang 18. Clang 19
  and 20 build them.
- `stdexec-tx` holds patches to stdexec, not programs, so it isn't built.

To build with a local stdexec checkout instead of fetching one, set
`FETCHCONTENT_SOURCE_DIR_STDEXEC_<COMMIT>`, for example
`-DFETCHCONTENT_SOURCE_DIR_STDEXEC_30ACDB0=$HOME/src/stdexec`. The commits are
listed in `cmake/stdexec.cmake`. PR #2401's head, e72e0fa, is fetched from the
PR's branch in my fork, so it can be fetched only while that branch still
contains it.

For ThreadSanitizer, which `frames-stress` also reports, configure without a
preset and pass `-DBEMAN_BUILDSYS_SANITIZER=TSan` and a toolchain file from
`infra/cmake`. To repeat a README's measurements, use the flags it gives;
the Release presets' `-O3` isn't what most of them used.

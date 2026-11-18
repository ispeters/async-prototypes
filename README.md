# Async prototypes

Experiments behind my WG21 work on asynchronous functions, sender
interfaces, and an asynchronous calling convention for C++. Each directory is
one experiment and has its own README. The README says what the experiment
shows, what was measured, and how to build it.

I wrote these with Claude in a Claude project, where they lived as project
documents. They're here so they last and can be reached from outside the
project. Each was imported unchanged and then rebuilt to check that nothing
was lost in copying. Making them build from this repository without
per-experiment setup comes later.

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

Where a README names a file under `synthesis/` or `function-coroutines/`, that
file is a design note in the Claude project, not in this repository.

`sp-stack/async_abi.h` and `sp-stack/async_abi.c` are symbolic links to
`kv-handle`'s, which that README says to put beside its files. `kv-dest` and
`tony` carry their own variants of the ABI header.

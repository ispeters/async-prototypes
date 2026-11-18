# Direct calls to a separately compiled async entry

2026-11-14. An experiment for the asynchronous calling convention
(`synthesis/problem-statements-v1.md`, problem 2.2).

## Question

Can a call to a separately compiled asynchronous function be a direct,
linker-resolved call, as a synchronous call is, and can that be shown without
compiler support?

Ian's suggestion: make the entry function a constant template argument of the
operation state.

## What the files do

- `callee.cpp` defines `get_entry`, an "async entry": it takes an argument, a
  continuation and a result slot, and returns `true` if it completed inline (the
  inline-completion handshake).
- `caller.cpp` only declares `get_entry`. `op_static<&get_entry>` takes the entry
  as a constant template argument. `op_dynamic` holds it as a run-time pointer, as
  a type-erased object does.

## Observed

x86-64 Linux, `-std=c++20`; `objdump -dr` on `caller.o`.

| | Entry as template argument | Entry as run-time pointer |
|---|---|---|
| GCC 13.3, `-O0` | `call` with `R_X86_64_PLT32 get_entry` | `call *%r8` |
| GCC 13.3, `-O2` | `call` with `R_X86_64_PLT32 get_entry` | `call *%rax` |
| GCC 13.3, `-O2 -fPIC` | `call` with `R_X86_64_PLT32 get_entry` | `call *%rax` |
| Clang 18.1, `-O0` | `call` with `R_X86_64_PLT32 get_entry` | `call *%rax` |
| Clang 18.1, `-O2` | `call` with `R_X86_64_PLT32 get_entry` | `call *%rax` |
| Clang 18.1, `-O2 -fPIC` | `call` with `R_X86_64_PLT32 get_entry` | `call *%rax` |

A call with a relocation against the callee's symbol is the form any call to an
external function takes. The linked program (`g++ -O2 caller.cpp callee.cpp`) exits
with 0.

Not tried: MSVC, nvc++, other architectures.

## What it shows, and doesn't

- The forward direction of an asynchronous call can be direct in a library
  prototype, when the callee is named where the operation state's type is formed.
- The handshake works as a return convention: on the inline path the call is an
  ordinary call and return.
- The completion after a real suspension still goes through a pointer.
- Not shown: emitting the entry automatically for every function that returns
  `function`; recognizing the named callee implicitly at an await; agreement across
  compilers on the entry's signature.

## Where the constant can live (inference)

`function<Sig>` is one type for every callee, so its `connect` has one return type.
The constant has to enter through a type the caller sees:

- at the call site, in a separate sender type whose `connect` returns the
  parameterized operation state; or
- in the return type, as an extra type argument that names the entry and converts to
  the plain erased `function<Sig>`. That type is concrete and separately compiled
  without exposing the implementation's structure. Virtual functions still need the
  erased form.

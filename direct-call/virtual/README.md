# A virtual async entry, compared with a synchronous virtual call

2026-11-14. Follow-up to `direct-call/README.md`.

## Question

If the entry is a constant pointer to a *virtual* member function, does an
asynchronous call through an interface cost one vtable dispatch, as a synchronous
virtual call does, and is it devirtualized where the synchronous call is?

## What the files do

- `iface.hpp`: `store` declares a virtual `get_entry` (the async entry) and a
  virtual `get_sync` (the baseline). `remote` is a `final` implementation, defined
  in `impl.cpp`.
- `caller.cpp`: `op<Entry, Obj>` calls `(self.*Entry)(…)` from `start`. Three
  situations, each with its synchronous twin.
- `caller2.cpp`: two more ways to name the entry when the static type is `final`.

## Observed

x86-64 Linux, `-std=c++20 -O2`; GCC 13.3 and Clang 18.1; `objdump -dr`.

| Situation | Synchronous call | Async, entry `&store::get_entry` |
|---|---|---|
| Dynamic type unknown (`store&`) | one indirect jump through the vtable | one indirect call through the vtable |
| Static type `final` (`remote&`) | direct call to `remote::get_sync` | **indirect** through the vtable (both compilers) |
| Object constructed in view, used through `store&` | GCC: direct. Clang: indirect | GCC: direct call to `remote::get_entry`. Clang: indirect |

For the `final` case (`caller2.cpp`):

| Entry named as | Result (both compilers) |
|---|---|
| `&remote::get_entry` (the final class's own virtual function) | still indirect |
| a plain function making the qualified call `r.remote::get_entry(…)` | direct call to `remote::get_entry` |

The linked program (`g++ -O2 caller.cpp impl.cpp`) exits with 0.

## Reading

- With the dynamic type unknown, the async call makes one vtable dispatch, the same
  as the synchronous call.
- Where the compiler can see the object's construction, the async call is
  devirtualized exactly when the synchronous one is.
- Where only the static type's finality is known, a call through a pointer to a
  virtual member function isn't devirtualized, though the synchronous member call
  is. A library prototype can recover the direct call by naming a plain function
  that makes a qualified call. A compiler implementing the entry natively has the
  same information it uses for the synchronous call (inference).

Not tried: MSVC, nvc++, other architectures, other optimization levels,
whole-program devirtualization.

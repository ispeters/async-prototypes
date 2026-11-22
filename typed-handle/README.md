# Typed operation handles (2026-11-22)

D4481's `operation_handle` is one erased type: whatever `start` or a
completion hands back to be run next. This experiment adds a typed handle
beside it, by analogy with `coroutine_handle<Promise>` and
`coroutine_handle<>`:

- `operation_handle<R, Sig>` delivers completion `Sig` to a receiver of type
  `R`. It holds a copy of the receiver and, when run, calls
  `r.complete<Sig>()`. The pair is what `how` says in the erased form, lifted
  from values into types.
- `operation_handle<Op, start_t()>` delivers `start_t()` to an operation
  state: it holds `Op*` and, when run, calls `op->start()`. So one family,
  "deliver `Sig` to a `T`", covers starting and completing.
- `operation_handle<>` is the erased form: two words, a function and a data
  pointer, the function delivering one signal to one target. A typed handle
  converts to it implicitly, if its target fits in a pointer.
- `no_operation` is the typed "nothing to run".

Because the type of a typed handle names the code it runs, a chain of them
can be followed, and inlined, at compile time. The question was whether that
gives the stack safety of deferring every transfer of control without the
cost of erasing every one, and whether the types stay manageable, which
Robert Leahy found they didn't on his `trampoline_20260511` stdexec branch.

Everything under "Observed" was run or measured. "Reading" is inference.

## Files

| File | What |
|---|---|
| `handle.hpp` | The handles, `start_t`, following a chain at compile time (`settle`), running a loop of known types as a loop, the driver (`run`), common types, `either`, and the four modes |
| `env.hpp` | Environments, `never_stop_token`, and a simplified `inplace_stop_source` with callbacks |
| `algos.hpp` | `just`, `just_unless_stopped`, `then`, `let_value`, `when_all`, `repeat_until`, an erased boundary, and a root, written once for every mode |
| `checks.cpp` | Results, errors, an immovable value, stopping, a loop's stack depth, and which handle types come back |
| `bench.cpp`, `cg.sh` | Scenarios, and instruction counts under Callgrind |

## Four modes

Each program is built with one `TH_MODE`. Only `later()` and `start_later()`
in `handle.hpp`, which defer a delivery from inside another, the common type
of two handles, and one named return type in `repeat_until` (see 10 below)
depend on it.

| Mode | A delivery from inside another | Where two outcomes meet |
|---|---|---|
| `EAGER` (0) | is a plain call, nesting as C++26 does; the closest to `tony/`'s C++29 column, which erases only at boundaries | — |
| `ERASED` (1) | returns `operation_handle<>` | erased |
| `TYPED` (2) | returns the typed handle | erased |
| `VARIANT` (3) | returns the typed handle | a `choice<Hs...>` of up to four typed handles, with an index |

Values never travel with a handle. As in `tony/`, a value is constructed at
the receiver's destination, then the completion is delivered, so a completion
handle is only the receiver, and receivers here are one pointer to their
parent.

## Building

`cmake --workflow --preset gcc-debug` builds and runs everything with the
rest of the repository; `ctest -R typed_handle` runs just these. By hand:

```sh
for m in 0 1 2 3; do
  g++ -std=c++23 -fsanitize=address,undefined -DTH_MODE=$m checks.cpp -o c$m && ./c$m
done
./cg.sh g++ 1000; ./cg.sh clang++ 1000
```

Measured with GCC 13.3 and Clang 18.1 at `-O2`, statically linked, x86-64.
The checks also ran under ASan and UBSan with GCC, and in the `gcc-debug` and
`llvm-release` presets.

## Observed

### Starting and completing are one family

`repeat_until` restarts its child by returning
`operation_handle<child_op, start_t()>` from its receiver's completion: a
deferred start, not a custom edge. Robert's branch needed a dedicated
`__start_again` continuation for this. `let_value` starts its second child
the same way. No algorithm here needed a handle other than "deliver `Sig` to
a `T`".

### Instructions per run

Callgrind, the difference between 1000 and 3000 runs; for `repeat`, per
iteration of a 1000- and a 3000-iteration loop. "Indirect" is indirect
branches beyond the one with which `bench` calls each scenario. Every
scenario completes inline.

| Scenario | EAGER | ERASED | TYPED | VARIANT | Indirect: E / Er / T / V |
|---|---|---|---|---|---|
| `chain4`: four `then`s that can't throw | 9 / 8 | 98 / 89 | 9 / 8 | 9 / 8 | 0 / 4 / 0 / 0 |
| `chain4_throwing`: four `then`s that may throw | 42 / 35 | 170 / 153 | 149 / 143 | 80 / 40 | 0 / 4 / 3–4 / 0 |
| `let`: `let_value` starting a `just` | 9 / 8 | 79 / 68 | 9 / 8 | 9 / 8 | 0 / 3 / 0 / 0 |
| `both`: `when_all` of two `then`s | 71 / 70 | 122 / 115 | 81 / 89 | 88 / 73 | 0 / 3 / 0–1 / 0 |
| `boundary`: `then` around an erased boundary around `then` | 82 / 69 | 106 / 91 | 82 / 71 | 82 / 71 | 2 / 5 / 2 / 2 |
| `repeat`: per iteration | 15 / 24 | 51 / 52 | 37 / 40 | 33 / 32 | 0 / 3 / 1 / 0 |

Each cell is GCC / Clang. In the indirect column, where GCC and Clang differ
the cell gives a range, GCC's count first.

1. **A typed chain costs what nesting costs.** Where only one outcome is
   possible, TYPED and VARIANT compile to the same instruction counts as
   EAGER, with no indirect calls: `chain4` and `let` fold to a handful of
   instructions in every mode but ERASED.
2. **Erasing every delivery is expensive:** ERASED runs from 1.3 times
   (`boundary`) to 11 times (`chain4`) EAGER's instructions, with one
   indirect call per hop.
3. **Erasing where outcomes meet is nearly as expensive, on the path that
   is taken.** A `then` whose function may throw has two outcomes, so in
   TYPED mode its completion is erased. Nothing throws in `chain4_throwing`,
   yet it costs 149 / 143 instructions against EAGER's 42 / 35, close to
   ERASED. The type is erased whichever outcome happens, so the common path
   pays too. (Before measuring, Claude had expected this cost to be small
   because the error side is cold. It isn't.)
4. **A bounded choice recovers most of it.** In VARIANT mode the same
   completion is `choice<value handle, error handle>`, and following it
   branches on an index, each branch carrying on at compile time:
   `chain4_throwing` is 40 with Clang (EAGER 35) and 80 with GCC (EAGER 42),
   with no indirect calls. With `std::variant` as `choice`'s storage GCC took
   99; a hand-written tagged union is what's measured. The rest of GCC's gap
   is unexplained.
5. **A loop of known types runs as a loop.** When following a chain reaches
   a typed handle whose own chain can come back to it (a type-level check,
   `is_loop_head` in `handle.hpp`), the driver runs that handle in a `for`
   loop: each trip follows the chain from it at compile time and stops when
   the chain is back at it, and only leaving the loop is erased. This is
   Lewis Baker's `resume_continuation` in `continuation.hpp`, applied to
   typed handles. It needs the loop's branch to be typed: in VARIANT mode
   `repeat_until`'s completion is a `choice` of ending and restarting, so the
   whole cycle is known, and an iteration costs 33 / 32 instructions with no
   indirect call (before the static loop: 49 / 37, one indirect call). In
   TYPED mode the branch is erased, so there is no typed cycle to find; an
   iteration still costs 37 / 40 with one indirect call. Nesting costs
   15 / 24, but only because both compilers turn its recursion into a
   sibling call (see 8); GCC also unrolls it four times. In VARIANT mode GCC
   keeps the slots' engaged flags in memory on every trip.
6. **An erased boundary costs the same in TYPED and VARIANT as in EAGER:**
   the two indirect calls are the boundary's own (the record's destination
   function, and its resume function for the completion that happened; the
   callee's start is a direct call to a `noinline` entry). ERASED adds
   three.
7. **The optimizer sometimes sees through erasure inside one translation
   unit.** In TYPED mode `when_all`'s `arrive` returns an erased handle, yet
   GCC's `both` makes no indirect call; Clang's makes one. ERASED's counts are
   therefore a lower bound on what separately compiled code would pay.

`both` includes constructing `when_all`'s own stop source, which it does
even under a parent that can't be stopped (3 or 4 instructions; see 12).

### The stack

`repeat_until` counting to 10 and to 1000, measuring the extent of the
machine stack from inside the loop's body:

| Build | EAGER | ERASED | TYPED | VARIANT |
|---|---|---|---|---|
| `-O0`, ASan and UBSan (GCC) | 5,184 B; 575,424 B | 0 B; 0 B | 896 B; 896 B | 0 B; 0 B |
| `-O2` (GCC, Clang) | 16 B / 32 B, at 1000 | 0 B | 0 B | 0 B |

8. **Deferring bounds the stack; nesting bounds it only when the optimizer
   turns the nested calls into tail calls.** At `-O2` both compilers made
   EAGER's restart a sibling call, so its stack stayed flat and it ran the
   fewest instructions per iteration; at `-O0` it grew by about 575 bytes per
   iteration. The deferred modes' extent doesn't depend on the number of
   iterations in either build. (Reading: TYPED's 896 bytes are the
   difference between the first iteration, which runs inside the driver's
   compile-time chain from `start`, and the rest, which run from its loop.
   Not checked. In VARIANT mode every iteration runs from the static loop.)

### Types

9. **The types stay small.** A typed handle is one pointer. A `choice` is at
   most four of them plus an index. `when_all`'s `arrive` is the largest:
   nothing, value, stopped or error. The common type of two different typed
   handles is erased (TYPED) or a `choice` (VARIANT), so no algorithm threads
   an open set of continuation types through its branches, as Robert's branch
   had to.
10. **Typed handles break return-type deduction cycles; nesting doesn't.**
    In EAGER mode `repeat_until`'s `again()` can't deduce its own return type,
    because a restart nests a call that ends in `again()`; it has to be named
    (`again_t` in `algos.hpp`). In the other modes `auto` works, because the
    restart is a handle type named without deducing what running it returns.
    A compile-fail test (`typed_handle.deduced.0`) checks the first, and
    three programs that must compile (`deduced.1` to `deduced.3`) the
    second.
11. **The algorithms are no longer than `tony/`'s:** `then_op` is 65 lines
    against 66, `let_value_op` 81 against 89, `when_all_op` 89 against 94.
    That comparison is loose: `tony/` has environments and a by-reference
    `set_value` path, and this has typed branches for functions that can't
    throw. Returning `auto` and calling `either()` where outcomes meet is all
    an algorithm does differently from one returning the erased handle.

### Environments and stop tokens

Receivers forward their parent's environment through `get_env()`;
`when_all` gives its children its own stop token and, when the parent's
token can be stopped, forwards a stop request with a callback registered on
it; the erased boundary forwards the stop token, and only that query,
through a function in the record's table. `just_unless_stopped` is a leaf
that asks its stop token, completing stopped if stop has been requested.
Each `_checked` scenario is its namesake with that leaf, under a root whose
`inplace_stop_source` is never stopped; `both_live` is `both` under that
root, with leaves that don't ask. Same measurements as above.

| Scenario | EAGER | ERASED | TYPED | VARIANT | Indirect: E / Er / T / V |
|---|---|---|---|---|---|
| `chain4_unstoppable`: `chain4`'s leaf asks, but there is no token | 9 / 8 | 98 / 89 | 9 / 8 | 9 / 8 | 0 / 4 / 0 / 0 |
| `chain4_checked` | 52 / 56 | 104 / 99 | 67 / 75 | 59 / 65 | 0 / 5 / 1 / 0 |
| `both_live` | 155 / 131 | 203 / 174 | 165 / 153 | 157 / 137 | 0 / 3 / 0–1 / 0 |
| `both_checked` | 167 / 144 | 212 / 188 | 199 / 176 | 174 / 147 | 0 / 3–5 / 1–3 / 0 |
| `boundary_checked` | 104 / 90 | 125 / 110 | 109 / 97 | 111 / 88 | 3 / 6 / 4 / 3 |
| `repeat_checked`: per iteration | 39 / 44 | 60 / 61 | 54 / 55 | 45 / 42 | 0 / 3 / 2 / 0 |

12. **Forwarding environments costs nothing where nothing asks.** Adding
    `get_env()` to every receiver left every scenario's counts unchanged
    except `both`, whose `when_all` now constructs a stop source of its own.
    A leaf that asks a token which can't be stopped compiles to `just`:
    `chain4_unstoppable` costs what `chain4` does, because
    `never_stop_token::stop_possible()` is a constant.
13. **Every leaf that can be stopped is a place where outcomes meet.** Asking
    a live token adds 43 / 48 instructions to `chain4` in EAGER mode. In TYPED
    mode the leaf's branch (stopped, or a value) is erased: +58 / +67 and an
    indirect call. In VARIANT mode it's a `choice`: +50 / +57, no indirect
    call. So stop tokens make points 3 and 4 above matter more, not less.
14. **Forwarding a parent's stop into `when_all` costs more than the rest of
    `when_all`:** 61 to 84 instructions (`both_live` against `both`), in
    every mode, for registering and deregistering one callback, which here is
    two round trips through a spin lock. The leaves' own checks then add
    only 9 to 34 more (`both_checked`).
15. **Across the erased boundary the stop token costs one indirect call,**
    the query function in the record's table, and 21 or 22 instructions in
    EAGER mode (`boundary_checked` against `boundary`).
16. **A loop that asks every iteration** costs 24 / 20 more per iteration in
    EAGER mode, 12 / 10 in VARIANT mode, where the static loop still runs
    with no indirect call, and 17 / 15 in TYPED mode, with a second indirect
    call per iteration.
17. **Receivers stay one pointer.** The root's stop token lives in its state,
    not in its receiver: a receiver holding a token as well would be two
    words, and couldn't be erased into `{function, data}` (the conversion's
    `static_assert`). Everything an environment holds has to live in an
    operation state, as `when_all`'s stop source does.

## Reading

- The typed handle earns its keep where a delivery has exactly one possible
  outcome, which is most pass-through layers and every function that can't
  throw: there it costs what nesting costs, without relying on the optimizer
  for stack safety.
- Stop tokens cost little where nothing can stop, and where something can,
  the dominant cost is registering callbacks to forward a stop, not asking
  for one. Each stoppable leaf adds a place where outcomes meet, which makes
  how those are handled (erase or choose) more consequential.
- Where outcomes meet, erase or branch is a real choice: erasing keeps the
  types trivial and costs about as much as erasing everything; a bounded
  `choice` costs little (with Clang) and keeps the types small. Robert's
  explosion came from an unbounded set; bounding it at a few alternatives and
  erasing beyond that looks enough for these algorithms.
- Loops cost more than the optimizer's best case for nesting, but with a
  bounded `choice` at the loop's branch they need no indirect call, and
  unlike nesting their stack stays flat without the optimizer's help. That
  makes typed branches (VARIANT) worth more than erased ones (TYPED) twice
  over: on the path through the branch, and around loops.
- The pair `<R, Sig>` maps onto the erased form cleanly. Here the erased form
  is `{function, data}` with one function per `(T, Sig)`, so there is no
  `how` at all; converting to `{record, how}` instead would need every
  receiver's target to start with a record.

## What it doesn't show

- Nothing completes asynchronously: no `run_loop`, no threads. An
  asynchronous completion arrives through whatever queues it, which holds
  erased handles, so typed handles matter only for what runs inline after it.
- Only the stop token is forwarded across the erased boundary; no other
  query, and no scheduler affinity.
- Nothing requests stop from another thread, and `inplace_stop_source` is
  simplified (callbacks run under its spin lock), so registration costs are
  indicative, not stdexec's.
- No comparison with the `{record, how}` erased layout, which `kv-dest`
  measured, nor with Robert's branch itself (it isn't built here).
- The erased boundary is in the same translation unit as its callee, behind
  an optimization barrier and a `noinline` entry; a real separately compiled
  callee wasn't built.
- Sanitizer builds and Clang's `-O0` weren't measured for stack extent.

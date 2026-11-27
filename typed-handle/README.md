# Typed operation handles (2026-11-22, generalized 2026-11-23)

D4481's `operation_handle` is one erased type: whatever `start` or a
completion hands back to be run next. This experiment adds a typed handle
beside it, by analogy with `coroutine_handle<Promise>` and
`coroutine_handle<>`:

- `operation_handle<Op>` starts an operation state: it holds `Op*` and,
  when run, calls `op->start()`.
- `operation_handle<R, Tag(Args...)>` completes a receiver of type `R` with
  that signature. It holds a `void*` to the operation that contains the
  receiver, and when run calls `R`'s private `resume<Sig>` on it. The
  receiver itself is never copied: the standard doesn't require receivers to
  be copyable, and the child that holds one may be gone by the time the
  handle runs. The pair is what `how` says in the erased form, lifted from
  values into types.
- `operation_handle<R, Op>` is what a receiver of type `R`, contained in an
  operation of type `Op`, derives from: a handle whose completion hasn't
  been chosen. It holds the `Op*`, and its `complete<Sig>()` returns the
  handle for `Sig` without running anything; only it turns that handle's
  `void*` back into an `Op*`. A receiver writes `resume`, `destination` and
  `get_env`, never `complete`, so it can't do its operation's work before
  the child has returned. Which two-argument form is meant follows from
  whether the second argument is a function type, so `Op` needs no
  constraint, and may be incomplete, as it is for a receiver defined inside
  its operation.
- `operation_handle<>` is the erased form: two words, a function and a data
  pointer. Every typed handle converts to it implicitly, since every typed
  handle is one pointer to an operation state.
- `no_operation` is the typed "nothing to run".

Because the type of a typed handle names the code it runs, a chain of them
can be followed, and inlined, at compile time. The question was whether that
gives the stack safety of deferring every transfer of control without the
cost of erasing every one, and whether the types stay manageable, which
Robert Leahy found they didn't on his `trampoline_20260511` stdexec branch.

The algorithms handle any completion signatures the standard allows for
them: zero or several values per completion, references, several value
completions, any error types, and completion signatures that depend on the
environment. "What generality cost" below compares them with this
experiment's earlier, single-value versions (commit 524c718).

Everything under "Observed" was run or measured. "Reading" is inference.

## Files

| File | What |
|---|---|
| `handle.hpp` | The handles, following a chain at compile time (`settle`), running a loop of known types as a loop, the driver (`run`), common types, `either`, and the four modes |
| `payload.hpp` | What a completion carries and where it is constructed; signature lists; originating a completion (`set_value`, `set_error`, `set_stopped`) |
| `env.hpp` | Environments, `never_stop_token`, and a simplified `inplace_stop_source` with callbacks |
| `algos.hpp` | `just`, `just_error`, `just_stopped`, `just_unless_stopped`; `then`, `upon_error`, `upon_stopped`; `let_value`, `let_error`, `let_stopped`; `when_all` of any number of children; `repeat_until` (stdexec's, not the standard's); an erased boundary; a root. Written once for every mode |
| `checks.cpp` | Results, errors, references, several values and value completions, every channel of `then` and `let`, `when_all`, stopping, a loop's stack depth, completion signatures, which handle types come back, and many outcomes meeting (a five-way `when_all` arrival, a 257-way `choice`) |
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
| `VARIANT` (3) | returns the typed handle | a `choice<Hs...>` of every typed handle that can come back, with an index |

## Completions and their payloads

Values never travel with a handle. A completion `Tag(As...)` is delivered in
two steps: its payload is constructed at the receiver's destination for that
signature, then the completion is delivered. So a completion handle is only
a pointer to the operation that contains the receiver, with the receiver's
type and the signature naming the code to run.

| Signature | Payload at the destination |
|---|---|
| `Tag()` | nothing; there is no destination |
| `Tag(A)` | an `A` |
| `Tag(T&)`, `Tag(T&&)` | a `T*` |
| `Tag(A, B, ...)` | `values<A, B, ...>`, one aggregate per signature (D4471's unnamed aggregate), each element as above and constructed in place on its own |

The consumer gets the arguments back in their declared value categories (an
`A` as `A&&`, a `T&` as `T&`). Senders compute their completion signatures
with a static `get_completion_signatures<Env>()`, as `stdexec::
completion_signatures` lists, using `exec::transform_completion_signatures`
and `exec::concat_completion_signatures` from stdexec 30acdb0, the commit
`tony/` uses. Nothing else of stdexec is used.

## Building

`cmake --workflow --preset gcc-debug` builds and runs everything with the
rest of the repository; `ctest -R typed_handle` runs just these. By hand,
with `$STDEXEC` stdexec's `include` directory at 30acdb0:

```sh
for m in 0 1 2 3; do
  g++ -std=c++23 -isystem "$STDEXEC" -fsanitize=address,undefined \
    -DTH_MODE=$m checks.cpp -o c$m && ./c$m
done
./cg.sh g++ 1000; ./cg.sh clang++ 1000   # STDEXEC defaults to a preset's copy
```

Measured with GCC 13.3 and Clang 18.1 at `-O2`, statically linked, x86-64.
The checks also ran under ASan and UBSan with GCC, at `-O2` with both
compilers, and in the `gcc-debug` and `llvm-release` presets.

## Observed

### Starting and completing are one family

`repeat_until` restarts its child by returning
`operation_handle<child_op>` from its receiver's completion: a
deferred start, not a custom edge. Robert's branch needed a dedicated
`__start_again` continuation for this. `let_value` (and `let_error`,
`let_stopped`) starts its second child the same way. No algorithm here
needed a handle other than starting an operation or completing a receiver.

### Instructions per run

Callgrind, the difference between 1000 and 3000 runs; for `repeat`, per
iteration of a 1000- and a 3000-iteration loop. "Indirect" is indirect
branches beyond the one with which `bench` calls each scenario. Every
scenario completes inline.

| Scenario | EAGER | ERASED | TYPED | VARIANT | Indirect: E / Er / T / V |
|---|---|---|---|---|---|
| `chain4`: four `then`s that can't throw | 9 / 8 | 86 / 73 | 9 / 8 | 9 / 8 | 0 / 4 / 0 / 0 |
| `chain4_throwing`: four `then`s that may throw | 51 / 35 | 154 / 108 | 135 / 100 | 67 / 29 | 0 / 4 / 3–4 / 0 |
| `let`: `let_value` starting a `just` | 17 / 8 | 75 / 61 | 17 / 8 | 17 / 8 | 0 / 3 / 0 / 0 |
| `both`: `when_all` of two `then`s | 60 / 54 | 110 / 96 | 70 / 70 | 77 / 54 | 0 / 3 / 1 / 0 |
| `boundary`: `then` around an erased boundary around `then` | 70 / 58 | 85 / 71 | 70 / 58 | 70 / 58 | 2 / 4 / 2 / 2 |
| `repeat`: per iteration | 11 / 34 | 45 / 43 | 29 / 31 | 26 / 24 | 0 / 3 / 1 / 0 |

Each cell is GCC / Clang. In the indirect column, where GCC and Clang differ
the cell gives a range, GCC's count first.

1. **A typed chain costs what nesting costs.** Where only one outcome is
   possible, TYPED and VARIANT compile to the same instruction counts as
   EAGER, with no indirect calls: `chain4` and `let` cost the same in every
   mode but ERASED.
2. **Erasing every delivery is expensive:** ERASED runs from 1.2 times
   (`boundary`) to nearly 10 times (`chain4`) EAGER's instructions, with one
   indirect call per hop.
3. **Erasing where outcomes meet is nearly as expensive, on the path that
   is taken.** A `then` whose function may throw has two outcomes, so in
   TYPED mode its completion is erased. Nothing throws in `chain4_throwing`,
   yet it costs 135 / 100 instructions against EAGER's 51 / 35. The type is
   erased whichever outcome happens, so the common path pays too. (Before
   measuring, Claude had expected this cost to be small because the error
   side is cold. It isn't.)
4. **A choice recovers it with Clang, and most of it with GCC.** In
   VARIANT mode the same completion is `choice<value handle, error handle>`,
   and following it branches on an index, each branch carrying on at compile
   time: `chain4_throwing` is 29 with Clang (EAGER 35) and 67 with GCC
   (EAGER 51), with no indirect calls. `choice` is a hand-written tagged
   union; with `std::variant` as its storage, GCC did worse (measured on the
   earlier version). The rest of GCC's gap is unexplained.
5. **A loop of known types runs as a loop.** When following a chain reaches
   a typed handle whose own chain can come back to it (a type-level check,
   `is_loop_head` in `handle.hpp`), the driver runs that handle in a `for`
   loop: each trip follows the chain from it at compile time and stops when
   the chain is back at it, and only leaving the loop is erased. This is
   Lewis Baker's `resume_continuation` in `continuation.hpp`, applied to
   typed handles. It needs the loop's branch to be typed: in VARIANT mode
   `repeat_until`'s completion is a `choice` of ending and restarting, so the
   whole cycle is known, and an iteration costs 26 / 24 instructions with no
   indirect call. In TYPED mode the branch is erased, so there is no typed
   cycle to find; an iteration costs 29 / 31 with one indirect call. Nesting
   costs 11 with GCC, which turns its recursion into a sibling call, and 34
   with Clang, which doesn't (see 8).
6. **An erased boundary costs about the same in TYPED and VARIANT as in
   EAGER:** the two indirect calls are the boundary's own (the record's
   destination function, and its resume function for the completion that
   happened; the callee's start is a direct call to a `noinline` entry).
   ERASED adds three.
7. **`when_all`'s fan-in is a merge.** Its `arrive` has several outcomes
   (nothing yet, values, stopped, an error), so in TYPED mode it returns an
   erased handle: one indirect call per `when_all`, with both compilers. In
   VARIANT mode it is a `choice` of all of them. (In
   the earlier version GCC removed that indirect call; it no longer does.)

### The stack

`repeat_until` counting to 10 and to 1000, measuring the extent of the
machine stack from inside the loop's body:

| Build | EAGER | ERASED | TYPED | VARIANT |
|---|---|---|---|---|
| `-O0`, ASan and UBSan (GCC) | 10,656 B; 1,182,816 B | 0 B; 0 B | 976 B; 976 B | 0 B; 0 B |
| `-O0` (Clang) | 7,488 B; 831,168 B | 0 B; 0 B | 816 B; 816 B | 0 B; 0 B |
| `-O2`, at 1000 (GCC / Clang) | 32 B / 47,952 B | 0 B / 0 B | 32 B / 32 B | 0 B / 0 B |

8. **Deferring bounds the stack; nesting bounds it only when the optimizer
   turns the nested calls into tail calls.** At `-O2` GCC made EAGER's
   restart a sibling call, so its stack stayed flat; Clang didn't, and its
   stack grew by about 48 bytes per iteration (the earlier version's Clang
   build did make it a sibling call). At `-O0` it grew by about 1.2 KB per
   iteration with GCC and 830 bytes with Clang. The deferred modes' extent
   doesn't depend on the number of iterations in any build, and can't: every
   hop returns a handle to the driver before the next one runs, so the depth
   is bounded by one hop's chain, which following stops at the first type
   already seen. Optimization and sanitizers change the size of that bound,
   not whether there is one. (Reading: TYPED's constant extent is the
   difference between the first iteration, which runs inside the driver's
   compile-time chain from `start`, and the rest, which run from its loop.
   Not checked. In VARIANT mode every iteration runs from the static loop.)

### Types

9. **The types stay small.** A typed handle is one pointer. A `choice` is
   one alternative's storage plus an index, a byte up to 256 alternatives, so
   a `choice` of one-pointer handles is two words however many there are
   (checked for 257). The common type of two different typed handles is
   erased (TYPED) or a `choice` (VARIANT), so no algorithm threads an open set
   of continuation types through its branches, as Robert's branch had to:
   the alternatives are the outcomes of one place where outcomes meet, which
   its completion signatures bound. `when_all` with errors of three types
   arrives in one of five ways, and in VARIANT mode that's a five-way
   `choice`.
10. **Typed handles break return-type deduction cycles; nesting doesn't.**
    In EAGER mode `repeat_until`'s `again()` can't deduce its own return type,
    because a restart nests a call that ends in `again()`; it has to be named
    (`again_t` in `algos.hpp`). In the other modes `auto` works, because the
    restart is a handle type named without deducing what running it returns.
    A compile-fail test (`typed_handle.deduced.0`) checks the first, and
    three programs that must compile (`deduced.1` to `deduced.3`) the
    second.
11. **A `choice` needs no limit on its alternatives; what it costs is
    compile time.** The first version capped a `choice` at four alternatives
    (`TH_MAX_CHOICE`) and erased beyond that; nothing recorded why four. No
    check or scenario here reached the cap: lifting it left every mode's code
    unchanged with Clang, and with GCC changed only VARIANT's block layout,
    with the same instruction and indirect-branch counts in every scenario.
    The limits left are the compilers':
    - *Fold expressions.* Clang refuses a fold over more than 256 operands
      by default, so lookups in a pack of alternatives go through an array
      instead (`pack_index`, `any_of` and `all_of` in `handle.hpp`).
    - *Instantiation depth.* `visit` halves its range at each step, so its
      depth grows with the logarithm of the number of alternatives.
      `common_handle_t` still folds its arguments one at a time, so one
      `common_handle_t` naming about 900 handles exceeds GCC's default depth
      (failed at 1,000). An algorithm names a few.
    - *Compile time,* measured by compiling `checks.cpp` in VARIANT mode with
      a `choice` of N typed handles, each converted into it once and all
      settled in one function (GCC 13.3 / Clang 18.1, one run each):

      | N | Front end only | `-O2` |
      |---|---|---|
      | 2 | 1.7 / 2.4 s | 4.1 / 4.2 s |
      | 100 | 2.3 / 2.7 s | 5.3 / 6.1 s |
      | 200 | 3.3 / 5.6 s | 8.8 / 8.4 s |
      | 400 | 12.5 / 19.5 s | 35.6 / 25.5 s |

      It grows faster than the square of N. Forming the 400-way type took
      about a second; most of the rest was converting each handle into it,
      whose constraint and index each look through all N alternatives.
      Finding the index by deduction from base classes tagged with their
      positions saved about a third at N = 400 and was left out. Settling
      the same N-way `choice` at N call sites, where each is inlined, costs
      N × N branches to compile: an earlier version of the check took 150 s
      with GCC at N = 300.

### Environments and stop tokens

Receivers forward their parent's environment through `get_env()`;
`when_all` gives its children its own stop token and, when the parent's
token can be stopped, forwards a stop request with a callback registered on
it; the erased boundary forwards the stop token, and only that query,
through a function in the record's table. `just_unless_stopped` is a leaf
that asks its stop token, completing stopped if stop has been requested; it
declares `set_stopped_t()` only if its environment's token can be stopped.
Each `_checked` scenario is its namesake with that leaf, under a root whose
`inplace_stop_source` is never stopped; `both_live` is `both` under that
root, with leaves that don't ask. Same measurements as above.

| Scenario | EAGER | ERASED | TYPED | VARIANT | Indirect: E / Er / T / V |
|---|---|---|---|---|---|
| `chain4_unstoppable`: `chain4`'s leaf asks, but there is no token | 9 / 8 | 86 / 73 | 9 / 8 | 9 / 8 | 0 / 4 / 0 / 0 |
| `chain4_checked` | 42 / 43 | 92 / 82 | 55 / 55 | 47 / 43 | 0 / 5 / 1 / 0 |
| `both_live` | 144 / 116 | 190 / 155 | 154 / 132 | 146 / 116 | 0 / 3 / 1 / 0 |
| `both_checked` | 154 / 127 | 199 / 169 | 188 / 157 | 161 / 127 | 0 / 3–5 / 1–3 / 0 |
| `boundary_checked` | 88 / 80 | 105 / 89 | 98 / 83 | 99 / 80 | 3 / 5 / 4 / 3 |
| `repeat_checked`: per iteration | 31.5 / 40 | 54 / 52 | 47 / 46 | 38 / 33 | 0 / 3 / 2 / 0 |

12. **Forwarding environments costs nothing where nothing asks.** A leaf
    that asks a token which can't be stopped compiles to `just`:
    `chain4_unstoppable` costs what `chain4` does, because
    `never_stop_token::stop_possible()` is a constant. (When environments
    were added, the only scenario whose counts changed was `both`, by 3 or 4
    instructions: `when_all` constructs a stop source of its own even under
    a parent that can't be stopped.)
13. **Every leaf that can be stopped is a place where outcomes meet.** Asking
    a live token adds 33 / 35 instructions to `chain4` in EAGER mode. In TYPED
    mode the leaf's branch (stopped, or a value) is erased: +46 / +47 and an
    indirect call. In VARIANT mode it's a `choice`: +38 / +35, no indirect
    call. So stop tokens make points 3 and 4 above matter more, not less.
14. **Forwarding a parent's stop into `when_all` costs more than the rest of
    `when_all`:** 59 to 84 instructions (`both_live` against `both`), in
    every mode, for registering and deregistering one callback, which here is
    two round trips through a spin lock. The leaves' own checks then add
    only 9 to 34 more (`both_checked`).
15. **Across the erased boundary the stop token costs one indirect call,**
    the query function in the record's table, and 18 or 22 instructions in
    EAGER mode (`boundary_checked` against `boundary`).
16. **A loop that asks every iteration** costs 12 / 9 more per iteration in
    VARIANT mode, where the static loop still runs with no indirect call,
    and 18 / 15 in TYPED mode, with a second indirect call per iteration.
17. **Completions run against operation states, so receivers can be
    anything.** A completion handle points at the operation that contains
    the receiver, so its erased form is always `{function, op*}`, whatever
    the receiver holds. The root's receiver is move-only, and every
    algorithm moves the receiver it's given, so the checks would fail to
    compile if anything copied one; calling a receiver's `resume` from
    outside fails too. Two earlier versions differed. The first copied the
    receiver into the handle, which required receivers to be trivially
    copyable and one pointer wide, so the root kept its stop token in its
    state for that reason (it still does, now because that's where its
    completions run). The second had each receiver publish its operation's
    type and address and write a static `complete`, which did the work, so
    `later` had to build every handle from outside the receiver; that
    changed no indirect-branch count and moved instruction counts by -10 to
    +2. Having receivers derive from `operation_handle<R, Op>` changed
    nothing in TYPED or VARIANT mode. In ERASED mode it saved a trip
    through the driver at the erased boundary (`boundary`, 93 / 78 to
    85 / 71 instructions, one fewer indirect call): `later` used to wrap
    even the boundary's receiver, whose handle is already erased, in a
    thunk that only returned it, and now asks the receiver for its handle
    first, and erasing an erased handle changes nothing. The tables above
    are current.

### What generality cost

Against this experiment's earlier versions, in which each sender had one
value type, `then`'s function returned a value, errors were
`exception_ptr`, and `when_all` had two children:

18. **Lines.** `algos.hpp` grew from 734 to 1,192 lines; the new
    `payload.hpp` is 406 (signature lists, payloads, and originating
    completions); `handle.hpp` shrank from 527 to 510 (its completion
    helpers moved). By algorithm, counting each family once:

    | Algorithm | Before | After | Grew by |
    |---|---|---|---|
    | `just` (now with `just_error`, `just_stopped`) | 21 | 27 | multiple values; a throwing move becomes an error |
    | `then` (with `upon_error`, `upon_stopped`) | 69 | 77 | `void` and reference results; any channel |
    | `let_value` (with `let_error`, `let_stopped`) | 89 | 133 | a second operation per value completion; copying references; a destructor that must know which |
    | `when_all` | 126 | 285, plus 47 computing its signatures | any number of children; staging values that can't go straight in; every error type; destroying errors not handed on |
    | `repeat_until` | 96 | 100 | any value convertible to `bool` |
    | erased boundary | 44 | 53 | a table built from any signature list |

    Before building it, Claude estimated about 200 lines of signature
    machinery and `algos.hpp` roughly doubling.
19. **Compile time:** `bench.cpp` at `-O2` went from 1.7 s to 1.9 s with GCC
    and from 1.8 s to 2.7 s with Clang (one run each; Reading: mostly
    stdexec's headers).
20. **Run time didn't get worse, mostly.** Of the scenarios measured before,
    most cost the same or fewer instructions now; GCC's `let` went from 9 to 17 and its
    EAGER `chain4_throwing` from 42 to 51, and Clang's EAGER `repeat` from
    24 to 34 per iteration, because Clang stopped turning its recursion into
    a sibling call. (Reading: the general versions keep payloads in storage
    that the arriving completion identifies, with no engaged flag, where the
    earlier versions wrote one; the drop wasn't isolated.)
21. **Where the destination decisions showed:**
    - *One aggregate per signature:* `when_all` constructs a child's single
      non-reference value straight into its result, but a child with several
      values or a reference stages them and moves or copies them in when all
      have succeeded, as C++26's `when_all` copies. One destination per
      argument would have let every child's values go straight in. (The extra
      move isn't measured.)
    - *Pointers for references:* nothing needs a path of its own, except
      where a value must outlive its producer: `let_value` copies before its
      first child goes, `when_all` when all have succeeded.
    - *The completion says which payload is there,* so `then`,
      `repeat_until` and `when_all` keep no index for it while delivering.
      Destruction outside any completion still needs one: `let_value`'s
      destructor (which second operation, which kept arguments) and the
      root's.

## Reading

- The typed handle earns its keep where a delivery has exactly one possible
  outcome, which is most pass-through layers and every function that can't
  throw: there it costs what nesting costs, without relying on the optimizer
  for stack safety. Clang's EAGER loop (8) is a reminder that the optimizer
  can't be relied on.
- Stop tokens cost little where nothing can stop, and where something can,
  the dominant cost is registering callbacks to forward a stop, not asking
  for one. Each stoppable leaf adds a place where outcomes meet, which makes
  how those are handled (erase or choose) more consequential.
- Where outcomes meet, erase or branch is a real choice: erasing keeps the
  types trivial and costs about as much as erasing everything; a `choice`
  costs little and keeps the types small. Robert's explosion came from
  threading every reachable continuation type through every algorithm. What
  prevents it here is merging where outcomes meet, not a cap on the merge:
  a `choice` holds one meeting place's outcomes, which its completion
  signatures bound, and the cap of four never came into play. Its real
  limit is compile time, which grows quickly with width but is negligible at
  the widths these algorithms produce.
- Loops: with a `choice` at the loop's branch they need no indirect
  call, and their stack stays flat without the optimizer's help. That makes
  typed branches (VARIANT) worth more than erased ones (TYPED) twice over: on
  the path through the branch, and around loops.
- Generality is mostly paid in source, not at run time: most of the growth
  is in `when_all` and `let_value`, and in the payload layer every algorithm
  shares.
- The pair `<R, Sig>` maps onto the erased form cleanly. Here the erased form
  is `{function, data}` with one function per `(T, Sig)`, so there is no
  `how` at all; converting to `{record, how}` instead would need every
  receiver's parent to start with a record.

## What it doesn't show

- Nothing completes asynchronously: no `run_loop`, no threads. An
  asynchronous completion arrives through whatever queues it, which holds
  erased handles, so typed handles matter only for what runs inline after it.
- Only these algorithms are general: no schedulers, `continues_on`,
  `starts_on`, `bulk`, `split`, `into_variant` or the rest.
- Only `just_unless_stopped` has completion signatures that depend on the
  environment, and only through its stop token.
- Throwing copies are handled where the standard requires an
  `exception_ptr`, but a payload of several values that throws part-way
  through construction leaks the elements already built.
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

# The async stack as the machine stack

2026-11-16. A prototype of Frank Birbacher's idea: an asynchronous function's
frame is its ordinary stack frame, a strand is a stack, and the stack pointer is
the cursor. It is `kv-handle`'s visible entry with the cursor parameter removed,
measured beside that entry (here called "cursor") and the synchronous call.

x86-64 Linux only (System V ABI, inline assembly), GCC 13.3 (`-std=c++20`) and
Clang 18.1 (`-std=c++23`), libstdc++, `-fno-omit-frame-pointer`. No stdexec.
Needs `async_abi.h` and `async_abi.c` from `kv-handle/` beside these files.

Everything under an "Observed" heading was run or measured. "Reading" is
inference.

## The convention (`mstack.hpp`, `mstack.cpp`)

```cpp
struct next { cont* k; int how; };                       // two registers
struct cont { next (*resume)(cont*, int how) noexcept; void* sp; };
next entry(args..., slot* out, cont* k);                 // no cursor
```

- **Completing inline.** The entry returns `{k, how}` the way any function
  returns. Its frame is popped by its own epilogue.
- **Suspending.** The entry returns `{nullptr}` *without popping its frame*
  (`MS_LEAVE`: push the return address again, restore the caller's frame
  pointer, `ret`). The caller gets control back with the stack pointer below
  the callee's frame, and addresses its own locals through the frame pointer.
  A caller that isn't handed its own record back leaves the same way.
- **Resuming.** `cont::sp` records where the stack pointer stood in the
  function that owns the record. Whoever runs a continuation first loads that
  (`ms_run`), which pops everything below it. No frame is ever popped any other
  way after a suspension.
- **Strands.** `ms_enter(arg, sp, fn)` calls `fn` on another stack and comes
  back whether `fn` returned or left frames. `ms_run` does the same for a chain
  of continuations, and a continuation on another stack is reached by the same
  load of `cont::sp`. Both save the six callee-saved registers on the stack they
  were called on and keep that stack's pointer in `rbp`.
- **Forks.** Each child of a fork gets a stack from a `stack_provider`. The
  child's completion hands back the parent's record, and running it moves the
  stack pointer to the parent's stack. Two providers: `mmap_provider` (pages from
  the OS, a guard page below each, optionally pooled) and `arena_provider`
  (fixed-size pieces of a buffer the user supplies; no system calls, no guard).

A body, as hand-lowered here (`stores.cpp`):

```cpp
ms::next cache_m::get_entry(str& key, slot<value_t>* out, ms::cont* k) const noexcept {
  value_t& hit = *::new (&out->value) value_t(entries_.find(key));
  if (hit) return {k, ASYNC_VALUE};
  hit.~value_t();
  cache_frame_m f;                       // a local variable
  f.resume = &resumed;
  f.sp = MS_SP();
  /* ... */
  ms::next n = backing_->get_entry(f.t, out, &f);
  MS_CLOBBER();
  if (n.k != &f) MS_LEAVE(n);            // the callee suspended: leave our frame too
  return f.after_await(n.how);
}
```

## Emulation: what a compiler would do differently

- **Callee-saved registers.** A function that leaves its frame doesn't restore
  them, so every call to an entry is followed by `MS_CLOBBER`, an empty `asm`
  that tells the compiler they're gone. The cost is that the calling function
  saves and restores all five, and spills around the call. A compiler would
  give the convention no callee-saved registers. `-DMS_NO_CLOBBER` removes the
  marker to measure it, and is only safe when nothing suspends.
- **The record duplicates the frame.** The hand-written record holds copies of
  parameters the compiler also spills. A compiler's frame would hold each once.
- **Resuming runs in a second frame.** The code after an await is a separate
  function (`resumed`), so a function that suspends again after being resumed
  leaves that second frame too. A compiler would continue in the first.
- **The stack protector.** Ubuntu's default `-fstack-protector-strong` guards
  functions with an address-taken local, which is every machine-stack body and
  no cursor body.
- **The red zone.** A function that leaves its frame must keep nothing below
  the stack pointer. Every such function here makes a call, which rules it out
  with both compilers. Link-time inlining of `ms::defer` would break that.

## Observed: it works

`checks.cpp` passes with GCC and Clang at `-O0`, `-O1`, `-O2` and `-O3`, and
with GCC under UBSan (this machine's Clang has no sanitizer runtime; ASan wasn't
tried, see below). It covers:

- one get: hit, miss, absent key; backing store inline and suspending;
- a loop of 1000 awaits under one root, with the stack's high-water mark equal
  after 10 and after 1000 iterations;
- a caller that writes over 2 KiB of stack after its callee suspended, 102
  times, with the callee's frame intact each time;
- forks of 200 children, inline and suspending, with all three providers, twice
  each; the pooled provider maps 200 stacks in total;
- that the suspended frames were on the strand's stack.

`tree_bench` checks a 17-deep chain of suspended calls, each resumed in turn.

## Observed: one get (`bench.cpp`, `cg.sh`)

Instructions per get, callgrind, `-O2 -DNDEBUG`, static link; GCC, then Clang in
parentheses. Indirect branches are equal across the three columns in every row
of the first table (5, 9 and 12).

**n gets in a loop under one root** (the caller is `count_found`, itself an
async function, so this is the cost of a nested call):

| | Synchronous | Cursor | Machine stack | Machine, no `MS_CLOBBER` |
|---|---|---|---|---|
| Hit | 344 (344) | 331 (339) | 348 (338) | 344 (338) |
| Miss | 631 (569) | 651 (648) | 652 (633) | 636 (633) |
| Miss, suspending | – | 774 (764) | 792 (753) | – |

**Each get under a root of its own:**

| | Synchronous | Cursor | Machine stack |
|---|---|---|---|
| Hit | 371 (352) | 369 (372) | 430 (432) |
| Miss | 658 (595) | 691 (683) | 752 (727) |
| Miss, suspending | – | 819 (789) | 877 (846) |

Entering a strand costs 44 (GCC) or 61 (Clang) more instructions per root than
the cursor's root, and one more indirect branch.

Time, ns per get, best of 7 runs of 2 million, three sessions on a shared
two-core machine: the three columns overlap in every row (hit 14 to 24, miss 29
to 49, suspending 37 to 53). No difference between cursor and machine stack is
resolvable in time.

## Observed: a call tree with nothing in the bodies (`tree.cpp`, `cg_tree.sh`)

`walk` visits a full binary tree, depth 16, awaiting each child in turn: 131071
calls, half of them the null-child case. Instructions per call, GCC (Clang):

| | Synchronous | Cursor | Machine stack |
|---|---|---|---|
| As built | 12.5 (12.0) | 47.5 (40.5) | 53.0 (39.5) |
| `-fno-stack-protector` | 12.5 (12.0) | 47.5 (40.5) | 47.0 (39.5) |
| and `-DMS_NO_CLOBBER` | 12.5 (12.0) | 47.5 (40.5) | 40.5 (36.5) |

Time, ns per call, completing inline, over two sessions: synchronous 1.0 to 1.7;
cursor 2.2 to 3.9; machine stack 2.7 to 6.1 as built and 1.9 to 2.6 without the
stack protector and the marker. With every null child suspending: cursor 5.1 to
7.0, machine stack 6.1 to 8.1.

## Observed: stack use

| | Cursor | Machine stack |
|---|---|---|
| One get, suspended: bytes held | 128 B | 432 B (GCC), 336 B (Clang) |
| One get, suspended: deepest | 128 B | 632 B, 472 B |
| One get, hit: deepest | 0 B | 408 B, 360 B |
| Tree, per call, completing inline | 62 B | 174 B, 157 B |
| Tree, per call, suspended | 62 B | 256 B, 224 B (includes the second frame) |

Deepest stack use of one synchronous call made from a strand, after a warm-up
call (`limits depths`; GCC, Clang):

| Call | Bytes |
|---|---|
| The cache miss, synchronously | 520, 448 |
| `snprintf("%d")` | 1896, 1880 |
| `snprintf("%f")` of 3.14e100 | 3096, 3080 |
| `std::ostringstream << double` | 3880, 3752 |
| `std::sort` of 10000 strings | 3344, 1160 |
| `fopen`, `fgets`, `fclose` | 632, 616 |
| `std::regex_match`, `(a|b)*c` on 2000 characters | 1537656, 960944 |

The first call of a lazily bound function went about 3 KiB deeper (seen before
the warm-up was added: 3608 B for the cache miss with Clang).

## Observed: forks (`count_all`, 1000 children)

ns per child, three sessions. The cursor column is the naive thing: one 4 KiB
`aligned_alloc` slab per child. Nothing smarter exists for the cursor design yet.

| Children | Cursor, slab each | Machine, pooled `mmap` | Machine, fresh `mmap` | Machine, user's buffer |
|---|---|---|---|---|
| Complete inline | 57–81 | 44–51 | 3200–4500 | 38–53 |
| Suspend | 176–251 | 149–214 | 4800–5700 | 123–170 |

- 10000 suspended children hold 4.13 KiB each on 64 KiB `mmap` stacks, and
  8.10 KiB each with the cursor's slabs (`limits rss`).
- Mapping 64 KiB stacks with a guard page fails after 32745 of them
  (`limits maps`; `vm.max_map_count` is 65530 here, and each stack is two
  mappings). Mapping and touching one costs 4.0 to 4.7 µs, unmapping 2.3 to 2.9.
- `count_all_m` keeps its children's records in its own frame with `alloca`.
  That is 128 B per child, so a 1000-child fork takes about 125 KiB of the parent's
  stack before any child runs. The roots here are 1 MiB for that reason.

## Observed: overflow (`limits overflow-guard`, `overflow-arena`)

- A strand on a 16 KiB `mmap` stack calling a synchronous function about 32 KiB
  deep: `SIGSEGV` 64 to 80 bytes below the stack's low end. The guard page
  caught it.
- Two strands on adjacent 16 KiB pieces of a user's buffer, the first suspended
  in a get, the second calling a synchronous function about 20 KiB deep: the
  call returned normally, and 419 of the first strand's 432 suspended bytes
  (GCC; just over 300 of 336, Clang) had been overwritten. Nothing reported it.

## Reading

1. **The mechanism is sound, and less exotic than expected.** Leaving a frame is
   three instructions, switching stacks is a load, and compilers already cope
   with a call that moves the stack pointer once a frame pointer is in use. It
   survived eight compiler configurations unchanged.
2. **It doesn't buy speed over the cursor on real bodies.** With 300 to 650
   instructions of work per get the two are within 5% of each other in either
   direction, and both within 14% of the synchronous call.
3. **On empty bodies it buys 10% to 15% of the instructions, at best.** The best
   estimate of a compiler's output is the last row of the tree table: 36.5 to
   40.5 against the cursor's 40.5 to 47.5. Both remain three to four times the
   synchronous call's 12. So the frame allocation was never the main cost of an
   async call: the continuation record and the test of the returned handle are.
4. **A strand's stack holds about three times what the cursor's strand holds,**
   in both examples, some of it emulation. That ratio matters less than what
   comes next.
5. **A strand's stack has to be sized for its deepest synchronous callee, and
   the cursor's strand doesn't.** Formatting a number needs 2 to 4 KiB; one
   regex needed 1.5 MiB. With the cursor those bytes land on the thread's stack,
   which every strand on the thread shares.
6. **Forks are cheap in time if stacks are pooled, and limited in number if
   they're guarded.** About 32000 guarded stacks per process by default on
   Linux, a page each at minimum.
7. **Without a guard page, overflow is silent corruption of a neighbour.** A
   user-supplied buffer is the configuration embedded users would want, and it
   is the one with no protection.
8. **What stays user-configurable is where stacks come from,** at roots and
   forks. Where a frame goes within a strand isn't.

## What it doesn't show

- A hand-written sender between two of these functions. An ordinary function
  that calls an entry and returns normally after the callee suspended would pop
  the callee's frame. Every function between a strand's root and a suspension
  has to follow the convention; how C++26 senders would be bridged isn't built.
- A compiler emitting any of this.
- More than one thread; a strand resumed on a different thread.
- Exceptions, unwinding through left frames, debuggers, signal handlers.
- ASan, shadow stacks (CET), `-fsplit-stack`, Windows, AArch64, macOS.
- Cancellation, environments, or anything else `kv-handle` also leaves out.
- A cursor design with small per-branch arenas, which is the fair opponent for
  forks.

## Files

`mstack.hpp`, `mstack.cpp` (the convention, the two switches, the providers);
`common.hpp`; `stores.hpp`, `stores.cpp` (the three stores, `count_found`,
`count_all`); `roots.hpp`; `checks.cpp`; `bench.cpp`, `cg.sh`; `tree.hpp`,
`tree.cpp`, `tree_bench.cpp`, `cg_tree.sh`; `limits.cpp`; `build.sh`,
`build_tree.sh`.

```sh
gcc -O2 -c async_abi.c                       # from kv-handle/
./build.sh g++ -O2 checks checks.cpp && ./checks
./build.sh g++ "-O1 -g -fsanitize=undefined -fno-sanitize-recover=all" checks_ub checks.cpp && ./checks_ub
./build.sh g++ "-O2 -DNDEBUG" bench bench.cpp && ./bench 2000000
./build.sh g++ "-O2 -DNDEBUG -DASYNC_STATS" sizes bench.cpp && ./sizes sizes
./build.sh g++ "-O2 -DNDEBUG -static" bench_static bench.cpp && ./cg.sh ./bench_static
./build_tree.sh g++ "-O2 -DNDEBUG -DASYNC_STATS" tree && ./tree
./build_tree.sh g++ "-O2 -DNDEBUG -static -fno-stack-protector" tree_static && ./cg_tree.sh ./tree_static
./build_tree.sh g++ "-O2 -DNDEBUG -fno-stack-protector -DMS_NO_CLOBBER" tree_nc && ./tree_nc inline
./build.sh g++ -O2 limits limits.cpp
./limits depths; ./limits overflow-guard; ./limits overflow-arena
./limits rss m 10000; ./limits rss c 10000; ./limits maps 100000
```

Substitute `clang++` for `g++` throughout.

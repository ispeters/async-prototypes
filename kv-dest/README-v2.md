# Result destinations for a middle that filters (W-157), ten ways

2026-11-17 (v2). v2 adds what was decided after the measurements, and the
argument as a paper would make it (the last two sections before "What it
doesn't show"); the code and numbers are v1's, unchanged.

Robert Leahy's objection to D4481R0: a middle sender can hand its
caller's result slot to its child only when every completion passes through.
If it filters one completion (Ian's binary `parse_bool` applying `upon_error` to
a fallible unary one), today's single slot forces the middle to take the child's
value in storage of its own and move it on. This experiment builds five ways of
telling a callee where each completion's payload goes, by two ways of telling the
caller which completion happened, and measures all ten in one binary.

Everything under "Observed" was run or measured. "Reading" is inference.

## The grid (`proto.hpp`)

| Destinations | A: one `resume` plus `how` | B: one resume function per completion (W-149) |
|---|---|---|
| 1. Single slot: `out` points at one union (`kv-handle` as is) | 1A, the baseline | 1B |
| 2. Pointer table at `out`: one pointer per payload | 2A | 2B |
| 3. Pointer table in the record: no `out` | 3A | 3B: pairs of resume function and destination |
| 4. Static function table: the record points at static functions that return each destination | 4A: the record is one pointer to {resume, destination functions} | 4B: one pointer per completion to a static {resume, destination function} |
| 5. Split: a member template of a known type in composed code, row 2's table at an erased call | 5A | 5B |

In column B an entry returns the address of the unit in the caller's record for
the completion that happened, so the function there knows which completion it
is; the fall-through compares the returned handle with the caller's units in
order (value, error, stopped).

A middle that passes a completion through answers "where does it go?" with its
parent's answer: rows 1, 2 and 5 forward the pointer they were given; row 3
copies the parent's pointers into its own record; row 4's destination function
calls the parent's, so a lookup chains through every pass-through layer. A
middle that filters supplies storage of its own for that completion: row 1 its
child's whole slot, the others room for the error alone.

## The workloads (`bodies.hpp`, `callers.hpp`)

Every function is reached by a virtual call, the entry visible (`kv-handle`'s
fastest column), so the variant is the only difference. One body per function,
generic over the variant. The backing store completes inline or from the event
loop.

| Workload | Shape |
|---|---|
| `get`, hit / miss | `kv-handle`'s cache over a backing store: everything passes through; the cache reads the value it passed through |
| `get_or`, value / error | `try { co_return co_await backing_->fetch(key); } catch (...) { co_return fallback; }`, erased: the value passes through, the error is filtered |
| `or_else`, value / error | the same middle, but a template composed statically with the caller, above an erased `fetch` |
| 2 × pass, value / error | two erased layers that pass everything through and count completions, above `get_or` |
| immovable (`pinned.cpp`) | `get_or` with a value type whose move constructor is deleted |

## Observed

x86-64 Linux, GCC 13.3 (`-std=c++20`) and Clang 18.1 (`-std=c++23`), libstdc++.
No stdexec.

### The baseline matches `kv-handle`

Row 1A is `kv-handle`'s visible entry. Its `get` reproduces that column's moves
and frame storage exactly (hit: 1 key copy, 1 value copy, 0 B; miss: 3 and 2,
80 B), and its instructions within 3% (GCC hit 400 against 392, miss 767
against 779).

### Moves and frame storage

Identical in every variant except:

- Row 1 makes one more value move on every filtered value path (`get_or`,
  `or_else`, 2 × pass: 1 against 0). No other row moves a value anywhere.
- Frame storage, inline (deferred adds the backing store's 48 B):

| | 1A | 2A | 3A | 4A | 5A | 1B | 2B | 3B | 4B | 5B |
|---|---|---|---|---|---|---|---|---|---|---|
| Record | 8 B | 8 B | 24 B | 8 B | 8 B | 24 B | 24 B | 48 B | 24 B | 24 B |
| `get`, miss | 80 | 80 | 80 | 64 | 80 | 96 | 96 | 112 | 80 | 96 |
| `get_or` | 96 | 96 | 80 | 64 | 96 | 112 | 112 | 112 | 80 | 112 |
| `or_else` | 96 | 96 | 80 | 64 | 96 | 112 | 112 | 112 | 80 | 112 |
| 2 × pass | 288 | 288 | 304 | 256 | 288 | 336 | 336 | 368 | 304 | 336 |

### The immovable type

Rows 1A and 1B don't compile ("use of deleted function
`pinned::pinned(pinned&&)`", both compilers). The other eight compile, and
deliver 42 on the value path and the fallback 7 on the error path under ASan
and UBSan.

### Instructions and indirect branches per call

callgrind (`--branch-sim=yes`, `-O2 -DNDEBUG`, static link; the difference
between 1000 and 3000 calls). GCC, then Clang in parentheses. Returns aren't
counted as indirect branches.

| | Sync | 1A | 2A | 3A | 4A | 5A |
|---|---|---|---|---|---|---|
| `get`, hit | 422/6 (363/6) | 400/6 (398/6) | 408/6 (400/6) | 406/6 (399/6) | 405/7 (399/7) | 408/6 (400/6) |
| `get`, miss | 718/10 (585/10) | 767/10 (679/10) | 773/10 (680/10) | 774/10 (684/10) | 800/14 (694/14) | 773/10 (680/10) |
| `get_or`, value | 328/7 (292/7) | 481/8 (444/10) | 453/8 (417/9) | 450/8 (415/9) | 447/9 (413/11) | 453/8 (417/9) |
| `get_or`, error | – | 453/8 (438/9) | 467/8 (445/9) | 464/8 (443/9) | 462/9 (442/11) | 467/8 (445/9) |
| `or_else`, value | – | 357/6 (344/7) | 347/6 (315/6) | 334/6 (315/6) | 337/8 (314/8) | 341/6 (313/6) |
| `or_else`, error | – | 340/6 (340/6) | 356/6 (345/6) | 343/6 (345/6) | 347/8 (345/8) | 349/6 (342/6) |
| 2 × pass, value | 546/9 (486/13) | 773/10 (713/16) | 745/10 (686/15) | 746/10 (700/15) | 741/13 (686/19) | 745/10 (686/15) |
| 2 × pass, error | – | 745/10 (707/15) | 759/10 (714/15) | 760/10 (728/15) | 756/13 (715/19) | 759/10 (714/15) |

| | 1B | 2B | 3B | 4B | 5B |
|---|---|---|---|---|---|
| `get`, hit | 400/6 (401/6) | 408/6 (403/6) | 400/6 (403/6) | 406/7 (402/7) | 408/6 (403/6) |
| `get`, miss | 778/10 (679/10) | 785/10 (679/10) | 779/10 (684/10) | 786/14 (691/14) | 785/10 (679/10) |
| `get_or`, value | 488/8 (447/10) | 460/8 (419/9) | 457/8 (419/9) | 455/9 (415/11) | 460/8 (419/9) |
| `get_or`, error | 467/8 (447/9) | 481/8 (453/9) | 472/8 (453/9) | 476/9 (450/11) | 481/8 (453/9) |
| `or_else`, value | 365/6 (340/7) | 348/6 (313/6) | 337/6 (313/6) | 348/8 (310/8) | 342/6 (311/6) |
| `or_else`, error | 346/6 (342/6) | 356/6 (348/6) | 345/6 (348/6) | 357/8 (346/8) | 349/6 (345/6) |
| 2 × pass, value | 778/10 (717/16) | 750/10 (689/15) | 753/10 (697/15) | 751/13 (687/19) | 750/10 (689/15) |
| 2 × pass, error | 757/10 (717/15) | 771/10 (723/15) | 768/10 (731/15) | 772/13 (722/19) | 771/10 (723/15) |

The synchronous `get_or` is the value path only; its error path throws.

### Time

ns per call, best of 7 runs of 1 million calls, two repetitions, `-O2 -DNDEBUG`,
on a shared two-core machine. The two repetitions of the synchronous hit
differed by 25% with GCC, so differences under about 10% aren't findings. The
only consistent separations: row 1 on the `or_else` value path (GCC 24 against
19 to 20 for rows 2 to 5; Clang 23 against 17 to 19), and 4A on the GCC miss
and 2 × pass value paths (53 against 47 to 50; 52 to 55 against 47 to 50). The
full tables are `time_*.txt` in the build directory, not kept.

## Reading

1. **Robert's objection holds for the single slot and for nothing else built
   here.** Row 1 costs one move of each filtered value (10 to 29 more
   instructions than row 2 with this string; the only consistent timing
   difference) and can't return an
   immovable type through a filtering middle at all. Every other row constructs
   every payload in place, through any number of layers.
2. **Among rows 2 to 5 the cost is a wash.** Instruction counts sit within
   about 4% of each other on every workload, and the timings can't separate
   them. The choice between them has to be made on other grounds: what the
   entry's signature looks like, how big the record is, what a C implementation
   writes.
3. **Row 4's chaining is visible but cheap at this depth.** Each pass-through
   layer adds one indirect call per destination lookup (3 more indirect branches
   for 2 × pass; 4 for the cache miss, which looks its value up twice). Its
   records and frames are the smallest of all (16 to 32 B less), and it makes
   fewer stores, so its instruction counts come out level or lower except on
   the cache miss (3% more); GCC's timings put it up to 10% behind on the
   deepest paths, Clang's don't. Deeper pass-through
   chains would cost linearly; this experiment stops at three layers.
4. **The split saves a few instructions, not because the table disappears.**
   Claude predicted that the optimizer would eliminate row 2's table in
   statically composed code. It didn't: the middle keeps a pointer to its
   parent's table in its frame for the error path, so the table escapes. Row 5
   saves 2 to 7 instructions on `or_else` against row 2 by not building the
   caller's table and knowing the caller's layout. What remains in its favour
   is how it reads: "a receiver answers 'where do I construct a `T`?'" as a
   member template fits C++26 receivers.
5. **One resume function per completion (W-149) doesn't pay here.** Column B
   is level with column A or up to 14 instructions behind (GCC `get_or` error:
   467 to 481 against 453 to 467), with records 16 to 24 B bigger and frames to
   match. Claude's guess is that the branch it saves at a fall-through costs
   about what its extra stores do; the generated code wasn't examined. That
   answers W-149's question for these bodies, though not for bodies with more
   completions.
6. **Row 4 plus W-149 doesn't give a record of fixed size.** Claude had
   predicted it would. It doesn't: a handle that is one pointer
   has to point at something per completion, so 4B's record has one pointer
   per completion (24 B). Only 4A has a fixed record (one pointer, 8 B, the
   smallest here), and it pays with a load to reach `resume`.

## What was decided after the measurements (Ian, 2026-11-17)

- Row 1 (the single slot) is discarded: it can't return an immovable type
  through a filtering middle, and it moves every filtered value.
- Column B (one resume function per completion, W-149's direction) is
  discarded: it never wins and its records are bigger.
- 2A is not a separate candidate: 5A runs 2A's code at every erased call, so
  2A survives only as the split's erased half.
- The remaining candidates are 3A, 4A and 5A. Nothing measured separates them
  by more than inlining noise (about 4%).
- Ian's constraint on the ABI: losing the ability to link objects built by
  different compilers would be impractical. So a paper has to argue either
  (1) that the representation is the obvious, only good choice, so every
  compiler picks it; or (2) that one representation supports every desirable
  implementation behind it, so compilers keep their freedom. Variation across
  ISAs is acceptable; within one platform there must be one ABI.

## The argument, as a paper would make it

A paper introducing entry functions can repeat this to show how it arrives at
its recommendation. The first two steps are measured here; the rest is
reasoning that the experiments listed under step 3 and step 5 would test.

### Two decisions, not one

The ten variants mix two independent choices:

- **What statically composed code uses.** Where the sender knows its
  receiver's type, "where do I construct a `T`?" can be a member template of
  that type: free once inlined, and the natural source form for a C++26
  receiver (whose completions are already member templates). Rows 2 to 4 used
  their run-time mechanism in composed code only because that is how they were
  built here; any of them could use the member template there instead.
- **What an erased call passes.** A pointer table at `out` (row 2, the split's
  erased half), pointers in the continuation record (row 3), or a static
  function table (row 4).

The first decision is settled in favour of the member template, and it is the
part WG21 would standardize (with the semantics: every payload constructed in
place, never moved through a middle). The second is the ABI, which a platform
fixes, not the standard.

### Why 4A is the expected answer

Under the second of the two arguments above, 4A is the candidate: it's the only
design where the callee asks where a payload goes instead of reading a pointer.
The frame being completed into answers, so how it answers is that frame's
implementation choice, inside one ABI:

| A frame answers its destination function by | which behaves like |
|---|---|
| returning an address it knows (its own storage, or its caller's, statically) | the member template, as functions |
| loading a pointer it saved at set-up (a pass-through layer resolving once, up front) | 3A, without a record that grows per completion |
| calling its parent's function | 4A as built here (chaining) |

The pointer tables can't host the other strategies: the callee does a load, so
every pointer has to exist before the call.

Further reasons, most important first (inference):

1. It expresses destinations the pointer tables can't: one chosen or created
   when the result arrives (results emplaced straight into a container's next
   element; error storage allocated only on the error path). With a pointer
   table those need a staging slot and a move, the cost W-157 set out to
   remove.
2. It's the erased form of the source form: the destination functions are the
   receiver's member template instantiated once per declared completion, as a
   vtable is the erased form of virtual member functions.
3. The record is one pointer whatever the number of completions, and the
   static table can gain entries later (environment queries, stop state)
   without changing record layout, as vtables evolve.
4. It is plain C: a struct of function pointers.

Its price: an indirect call per payload constructed, which no implementation
behind this ABI can turn into a load.

### The decision tree

1. **Must a middle that filters construct results in place, with no move and
   immovable types allowed?** Yes (a requirement). Drops row 1. *Measured.*
2. **Does one resume function per completion pay for itself?** No, for these
   bodies. Drops column B. *Measured, with three completions.*
3. **With pass-through layers resolving once, up front, is 4A's indirect call
   per payload within noise of a pointer load?** If yes: **4A**, one ABI with
   every answering strategy left to the implementation (argument 2 above). *Not
   yet run; this is the deciding experiment.*
4. If not: **are destinations computed at completion important enough to pay
   for the call?** If yes, still **4A**. If no, choose a pointer table and
   accept a staging slot and a move in those cases. *A requirements question
   for the committee.*
5. **Between the pointer tables, which grows worst?** Many payload-carrying
   completions count against 3A (its record grows); more parameters count
   against the split (one more argument, and `get_or` here already uses all six
   x86-64 integer argument registers); deep pass-through chains count against
   3A's copying. *Measurable with three workloads this harness doesn't have
   yet: a deeper chain, a function with one more parameter, more completions.*

Step 3's answer can differ by platform: indirect calls cost more where
control-flow integrity is enforced (Intel IBT, Arm BTI, Clang CFI) or where
Spectre mitigations turn them into retpolines. So the tree may be walked per
platform and land on different leaves, which the constraint above allows.

### Corrections made along the way

Recorded so a paper doesn't repeat them.

- Claude predicted that 4A combined with column B would give a record of fixed
  size. It doesn't: a handle that is one pointer must point at something per
  completion. Only 4A has a fixed record.
- Claude predicted that the optimizer would eliminate row 2's table in
  statically composed code. It didn't, because the middle keeps a pointer to
  its parent's table for the error path (reading 4).
- Claude first proposed comparing one variant at a time to keep attribution
  clean. Ian pointed out that building them all in one binary gives the same
  attribution (each row and each column changes one thing), so they were built
  together.

## What it doesn't show

- A compiler emitting any of this. Every lowering is hand-written.
- Erasure through `function` (`hfn`). Every call is a virtual entry. The hidden
  entry adds an indirect branch and some instructions in `kv-handle`, the same
  for every row.
- Typed errors, several value signatures, or more than three completions. Row
  sizes and column B's costs grow with the number of completions.
- A middle that filters on a completion other than the error (row 4 and the
  tables handle it the same way; not run).
- Pass-through chains deeper than three layers, where row 4's lookups grow.
- Cancellation, threads, the suspended path's timing, MSVC or other
  architectures.
- The member template with row 3's or row 4's erased form (the combinations
  "Two decisions, not one" says are valid), and 4A with pass-through layers
  resolving once (step 3 of the tree).

## Files

`async_abi.h`, `async_abi.c` (from `kv-handle`, plus a live-frame counter under
`ASYNC_CHECK`; the C task queue is unused, replaced by `common.hpp`'s, which
runs the handle itself because the two columns' handles differ); `common.hpp`;
`proto.hpp` (the ten variants); `bodies.hpp` (the bodies, the interface);
`stores.cpp` (the erased definitions); `callers.hpp` (roots, `or_else`, the
rigs); `bench.cpp`; `checks.cpp`; `pinned.cpp`; `cg.sh`.

```sh
gcc -O2 -c async_abi.c -o abi.o
gcc -O2 -DASYNC_CHECK -c async_abi.c -o abi_check.o
gcc -O2 -DASYNC_STATS -c async_abi.c -o abi_stats.o
# checks (498 runs)
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -DASYNC_CHECK stores.cpp checks.cpp abi_check.o -o checks
ASAN_OPTIONS=detect_leaks=0 ./checks      # the rigs leak their stores on purpose
# counts
g++ -std=c++20 -O1 -DCOUNT_MOVES -DASYNC_STATS stores.cpp bench.cpp abi_stats.o -o count && ./count
# time, and callgrind
g++ -std=c++20 -O2 -DNDEBUG -static stores.cpp bench.cpp abi.o -o bench && ./bench 1000000
./cg.sh ./bench                           # or: bench loop <variant -1..9> <workload 0..7> <calls>
# the immovable type, one variant per build
for V in V1A V2A V3A V4A V5A V1B V2B V3B V4B V5B; do
  g++ -std=c++20 -DVARIANT=$V pinned.cpp abi.o -o pinned && ./pinned
done
```

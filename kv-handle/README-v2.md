# `kv_store::get` and a `start` that returns a handle

2026-11-15 (v2). v2 makes the operation-state adaptor generic, adds a `connect`
for handle-aware receivers that takes any sender, and carries the async-stack
cursor through today's `function` two ways; every number is re-measured in the new
binary. v1 trimmed `kv-three-ways-v2/` to the versions that matter and rebuilt it
around one protocol: an operation's `start` returns a handle to what runs next. The
keep-and-drop list was Ian's and Claude's of 2026-11-15; current decisions are in
`synthesis/handoff-2026-11-15c-handle-protocol.md`.

Built against stdexec
[5afe072b](https://github.com/NVIDIA/stdexec/commit/5afe072bc89627cf5e0b960ee588ee28259fca33),
the commit v3 used, so that the trim changes one thing. stdexec `main` has since
moved to
[25134753](https://github.com/NVIDIA/stdexec/commit/25134753930e64ee13491fa15e314e9375219f57);
nothing here was tried against it. x86-64 Linux, GCC 13.3 (`-std=c++20`) and
Clang 18.1 (`-std=c++23`), libstdc++.

Everything under an "Observed" heading was run or measured. Each "Reading" section
is inference.

## What changed from v3

- Dropped: the entry that returns an `int`; the handle run by a loop only; the
  second allocator row for the published version. Their v3 numbers are under
  "Record of what was dropped".
- New: `hfn`, a stand-in for `function` that is constructed the way P4223's is and
  generates its erased entry from the factory. It replaces v3's `hget_fn`, whose
  `get` had to name its entry.
- New: the compatibility scenarios, for the handle. v3 had them only for the `int`
  entry.
- Ported: the C implementation.
- All columns are now in one binary, so every number shares a baseline.

## The protocol (`async_abi.h`, C)

```c
typedef struct { async_cont* next; int how; } async_next;   /* two registers */
async_next entry(args..., slot* out, async_cont* k, async_sp sp);
struct async_cont { async_next (*resume)(async_cont* self, int how); };
```

An entry or a `resume` returns the continuation that is ready to run next, with how
the operation it waited for completed, or `{NULL}`. An operation completes by
returning its caller's record and never calls it. Whoever is at the bottom of the
machine stack runs what comes back; a caller handed its own record carries on in
place. `out` is uninitialized storage in the caller for the result, `k` is a record
in the caller's frame, and `sp` is the top of the caller's strand of the async
stack.

In sender terms (`handle.hpp`): the return value is what `start` returns, and
`out`, `k` and `sp` are what a handle-aware receiver offers. A sender is
handle-aware if connecting it to such a receiver gives an operation whose `start`
returns `async_next`. As built, that operation is a record of pointers that needn't
outlive the call to `start`; whatever the operation keeps, `start` pushes on the
async stack.

## The interfaces (`kv_store.hpp`)

```cpp
struct kv_store   { virtual get_fn  get(str key) const = 0; };   // today: P4223R2's function
struct kv_store_h { virtual hget_fn get(str key) const = 0; };   // the entry hidden
struct kv_store_v { virtual async_next get_entry(str& key, slot<value_t>* out,
                                                 async_cont* k, async_sp sp) const noexcept = 0; };
template <class Fn> struct kv_iface { virtual Fn get(str key) const = 0; };
```

`get_fn` is P4223R2's `function<sender_tag(str) const&, ...>`. `hget_fn` is
`hfn<value_t(str) const&>`. `kv_iface` is instantiated with today's `function`
declared to forward something through its erasure: `get_fn_alloc` names the strand
allocator's type in `queries<...>`, and `get_fn_cursor` names the cursor query.

## Bridging the two protocols (`handle.hpp`)

| Receiver | Sender | What connects them |
|---|---|---|
| Handle-aware | Handle-aware | The sender's own `connect` |
| Handle-aware | C++26 | `thunk::start`: the C++26 operation state goes in a frame on the async stack, with the inline-completion handshake |
| C++26 | Handle-aware | `legacy_op`, the operation-state adaptor: a C++26 operation state that keeps the sender and supplies a slot, a continuation record that calls `set_value` and its siblings, and a cursor |
| C++26 | C++26 | `ex::connect` |

`hconnect(sndr, handle_rcvr)` picks between the first two. `legacy_op` works for
any handle-aware sender that names its `value_type`; `hfn`'s C++26 `connect`
returns one, and `as_cxx26(sndr)` wraps a sender that has no C++26 `connect` of
its own.

The adaptor finds its cursor in this order: the receiver's answer to the
`get_async_stack` query (`common.hpp`), if not null; the receiver's frame
allocator, if that is a `strand_allocator`; otherwise it roots a stack for the
operation. The query is a forwarding query whose default answer is null, so every
environment answers it.

## `hfn`: the `function`-shaped constructor

```cpp
hget_fn cache::get(str key) const {
  return hget_fn(*this, std::move(key), [](cache const& self, str& key) noexcept {
    return cache_get{&self.entries_, self.backing_, &key};
  });
}
```

`hfn` holds the curried arguments and one erased entry. The constructor generates
the entry from the factory's type, and which entry depends on the sender the
factory returns:

| The factory returns | The generated entry |
|---|---|
| A handle-aware sender that keeps nothing (the factory took the arguments by reference) | Calls the factory, connects, starts. No frame. |
| A handle-aware sender that owns something | Pushes a frame for the sender on the async stack; pops it when the operation completes |
| A C++26 sender | Calls the factory, connects through the thunk, starts |

`hfn`'s own `connect` is chosen by the receiver. A handle-aware receiver gets the
record of pointers. A C++26 receiver gets the operation-state adaptor.

Differences from P4223R2's constructor, all visible in `handle.hpp`:

- The factory has to be stateless. (stdexec's `function` also accepts a function
  pointer.)
- The constructor takes the curried arguments as `Args&&`. Taking them by value
  costs one more move of each per call.
- A factory or a `connect` that throws becomes an error completion, because the
  entry is `noexcept` (`checks.cpp`).
- `hfn` has no queries, no `noexcept` signatures and no allocator of its own.

## The definitions (`stores.cpp`)

| Column | Interface | Cache and backing store |
|---|---|---|
| Synchronous | `kv_store::get_sync` | The yardstick |
| Published | `kv_store` | P4223R2's `cached_lookup` and `mem_store`, in today's `function` |
| Lowered to a C++26 sender | `kv_store` | The direct-style body below, hand-lowered to a sender, in today's `function` |
| Hidden entry | `kv_store_h` | The body hand-lowered to an entry; `get` as shown above |
| Hidden entry, owning factory | `kv_store_h` | The same entry; the factory takes `str key` by value, as P4223 publishes its factories, and returns a sender that owns it |
| Visible entry | `kv_store_v` | The same entry as the virtual function |

```cpp
get_fn cache::get(str key) const {
  if (auto hit = entries_.find(key))
    co_return hit;
  auto value = co_await backing_->get(key);
  if (value)
    entries_.insert(key, *value);
  co_return value;
}
```

The entry form of each body exists once (`mem_body`, `cache_body`). The cache's is a
template over the backing store's interface, which decides only how
`co_await backing_->get(key)` lowers (`await_get`). Both are forced inline into
each use so that the hidden and visible columns run the same code.

## Observed: per call

Counts are from `-DCOUNT_MOVES -DCOUNT_HEAP -DASYNC_STATS` builds, one call each;
GCC under ASan and UBSan, and Clang without (this machine's Clang has no sanitizer
runtime). The two compilers agree except where noted. Instructions and indirect
branches are callgrind's (`--branch-sim=yes`, `-O2 -DNDEBUG`, static link; the
difference between 1000 and 3000 calls), GCC first and Clang in parentheses.
Returns aren't counted as indirect branches; calls to ifunc'd libc functions are.

Copies are the same in every column (hit: 1 key, 1 value; miss: 3 keys, 2 values).
No column calls `operator new`. The backing store completes inline.

**Hit** (the cache answers; one asynchronous call):

| | Synchronous | Published | Lowered to a C++26 sender | Hidden entry | Hidden, owning factory | Visible entry |
|---|---|---|---|---|---|---|
| Key moves | 0 | 7 | 5 | 1 | 3 | 0 |
| Value moves | 1 (GCC), 0 (Clang) | 6 | 3 | 0 | 0 | 0 |
| Frame storage | – | 304 B | 256 B | 0 B | 64 B | 0 B |
| Caller's own frame | – | 168 B | 168 B | 104 B | 104 B | 88 B |
| Instructions | 408 (349) | 1363 (1199) | 1013 (901) | 467 (465) | 605 (556) | 392 (379) |
| Indirect branches | 5 (5) | 14 (27) | 11 (19) | 6 (7) | 6 (9) | 5 (5) |

**Miss** (the cache calls the backing store; two asynchronous calls):

| | Synchronous | Published | Lowered to a C++26 sender | Hidden entry | Hidden, owning factory | Visible entry |
|---|---|---|---|---|---|---|
| Key moves | 0 | 16 | 10 | 2 | 6 | 0 |
| Value moves | 1 (GCC), 0 (Clang) | 7 | 7 | 0 | 0 | 0 |
| Frame storage | – | 448 B | 384 B | 96 B | 224 B | 80 B |
| Instructions | 732 (589) | 2484 (2010) | 2057 (1796) | 905 (821) | 1158 (1029) | 779 (684) |
| Indirect branches | 9 (9) | 26 (46) | 23 (40) | 11 (13) | 11 (17) | 9 (9) |

- With the backing store completing from the event loop, every count is unchanged
  except the three entry columns' frame storage, which grows by the backing store's
  48 B frame (144 B, 272 B, 128 B).
- An owning factory that takes `str&&` in place of `str` makes 2 key moves on a hit
  and 4 on a miss. Its frame storage is unchanged.
- Instruction counts depend on inlining decisions elsewhere in the binary. Making
  the adaptor generic changed no move, storage, slab or indirect-branch count in
  any v1 configuration, and moved GCC's instructions by 4 to 90 (hidden entry: 443
  and 877 in v1, 467 and 905 here) and Clang's by 0 to 24. Differences of that
  size between columns aren't findings.

**Time** (ns per call, best of 7 runs of 2 million calls, two repetitions, `-O2
-DNDEBUG`; a shared machine whose speed differs between sessions, so read the
ratios):

| | Synchronous | Published | Lowered to a C++26 sender | Hidden entry | Hidden, owning factory | Visible entry |
|---|---|---|---|---|---|---|
| GCC, hit | 19 | 80–81 | 57–58 | 23 | 31–32 | 20 |
| GCC, miss | 44–45 | 160–162 | 124 | 51 | 66 | 45 |
| Clang, hit | 15 | 65 | 44 | 20–21 | 25 | 16 |
| Clang, miss | 31 | 118–120 | 95–96 | 40–41 | 50 | 32–33 |

### Against the hand-off's predictions

The hand-off predicted, for the `function`-shaped constructor: "the hidden-entry
column's counts when the operation borrows the sender's arguments, one more key
move per call when it doesn't."

- Borrowing: moves, frame storage, caller's frame and GCC's indirect branches equal
  v3's `hget_fn` column exactly (1 and 2 key moves; 0 B and 96 B; 104 B; 6 and 11).
  Instructions don't: 467 and 905 against v3's 424 and 786. The generated entry is
  a function of its own (14 to 17 instructions per call when measured in v1) that
  GCC doesn't merge with the sender's `start`. That covers the hit's gap and under
  half of the miss's; the rest is unexplained, and v3's binary no longer exists to
  compare against.
- Owning: wrong as stated. A by-value factory costs two more key moves per call,
  not one (curried argument to factory parameter, parameter to sender); `str&&`
  gives the predicted one. The prediction also missed that the sender needs a
  frame even when the body doesn't: 64 B on a hit, where the borrowing form pushes
  nothing, and 138 (GCC) or 91 (Clang) more instructions on a hit.

## Observed: compatibility

Miss path unless marked. "C++26 root" is `ex::connect(s.get(key), rcvr)` then
`ex::start`, with a receiver that knows nothing of handles; "strand" means its
environment offers a strand as frame allocator and cursor, "plain" that it offers
nothing. Instructions and indirect branches are GCC's; time is GCC, then Clang.

| # | Scenario | Root | Cache | Backing store | Key / value moves | Instructions | Indirect branches | Time (ns) |
|---|---|---|---|---|---|---|---|---|
| – | Published (for reference) | C++26, strand | published | published | 16 / 7 | 2484 | 26 | 160–162, 118–120 |
| – | Hidden entry (for reference) | handle | new | new | 2 / 0 | 905 | 11 | 51, 40–41 |
| 1 | A new function awaits today's `function` | handle | new | published, behind `kv_store` | 8 / 3 | 1614 | 16 | 88–90, 65 |
| 2 | Today's code awaits a new function | C++26, strand | published, behind `kv_store` | new | 11 / 5 | 2568 | 19 | 156–158, 128 |
| 3 | A C++26 caller of new functions, offering a strand | C++26, strand | new | new | 3 / 1 | 1001 | 11 | 54–55, 45–46 |
| 4 | The same, offering nothing | C++26, plain | new | new | 3 / 1 | 1616 | 11 | 86, 72 |
| 5 | Published code recompiled with `hget_fn` for `get_fn` | handle | published | published | 17 / 4 | 2056 | 14 | 126–128, 96–97 |
| 6 | The same, from a C++26 caller | C++26, strand | published | published | 18 / 5 | 2152 | 14 | 132–133, 99–104 |

On a hit, scenario 1 costs what the hidden entry does (455 instructions, 1 key
move): the old callee isn't reached. Scenario 4 costs 1178 on a hit, against 563
for scenario 3.

- Scenarios 3 to 6 call the same `hfn` type through both kinds of receiver; so do
  the hidden-entry column and scenario 2, where the same `mem_n` object type is
  reached from a handle-aware cache and from `then`'s receiver.
- Scenario 2 roots one 4 KiB slab per call, although the root offers a strand.
  Today's `function` forwards only the queries its type names, and `get_fn` names
  none. Below it the frame allocator is stdexec's adaptor over
  `std::pmr::polymorphic_allocator<std::byte>` (seen in an instantiation trace),
  and the cursor query gets its null default. So the new callee roots a stack, as
  in scenario 4.
- `checks.cpp` runs every pairing with the backing store inline and deferred, for
  a present and an absent key, through both kinds of receiver, under ASan and
  UBSan; and a throwing factory through both.

## Observed: the cursor through today's `function`

Scenario 2 again, with the cache behind today's `function` declared to forward
the strand. Same columns; all from a C++26 root.

| # | `function` forwards | Root | Backing store | Slabs rooted | Frame storage | Instructions | Indirect branches | Time (ns) |
|---|---|---|---|---|---|---|---|---|
| – | Nothing (published, for reference) | strand | published | 0 | 448 B | 2484 | 26 | 160–162, 118–120 |
| 2 | Nothing | strand | new | 1 | 304 B | 2568 | 19 | 156–158, 128 |
| 7 | The frame allocator, by type | strand | published | 0 | 368 B | 2369 | 23 | 152–170, 104–106 |
| 8 | The frame allocator, by type | strand | new | 0 | 288 B | 1922 | 19 | 116–117, 87 |
| 9 | The cursor query | strand | published | 0 | 448 B | 2486 | 26 | 161–165, 108–109 |
| 10 | The cursor query | strand | new | 0 | 304 B | 1964 | 20 | 125–126, 93–95 |
| 11 | The cursor query | plain | new | 1 | – | 2772 | 20 | 170–172, 136 |

- Both ways carry the cursor across the erasure: no slab is rooted in rows 8 and
  10, and with a deferred backing store its 48 B frame appears on the root's
  strand. Moves are those of scenario 2 (11 key, 5 value).
- Row 10 against row 9: 21% fewer instructions with GCC and 15% with Clang (2007
  to 1711), where scenario 2 against the published version gains nothing.
- A `function` that names the allocator's type connects only to receivers whose
  frame allocator is that type (`checks.cpp`: it isn't `sender_to` a plain
  receiver). One that names the cursor query connects to any receiver, because
  the query has a default. In row 11 `function` allocates with `operator new`
  (1 call, 288 B) and the callee roots a slab.
- Either way the forwarded query is part of the `function`'s type, so of the
  declaration of `get`.

## Observed: what the bridge needs from a sender

- A handle-aware sender with no C++26 `connect` isn't an `ex::sender`
  (`checks.cpp`, `answer`). Wrapped in `as_cxx26` it composes with `then`. From a
  plain receiver the adaptor roots a stack; from one offering a strand it doesn't,
  so the cursor query passes through `then` with no declaration anywhere.
- Making the adaptor generic changed no count but instructions (see above).

## Observed: the implementation side in C

`c_store.c` is C11, compiled as C by GCC and Clang with `-Wall -Wextra -Wpedantic`.
It defines two entries for an interface whose types are C types (`c_store.h`): a
leaf that suspends, and a function that awaits the leaf, with the fall-through. Its
undefined symbols are `abort`, `aligned_alloc`, `free`, `strcmp`, `strlen`,
`async_defer` and a statistics counter. `c_demo.cpp` calls it through the entry and
as a sender composed with `then`; the output is v3's:

```text
entry  get_len("k2"): value 9 (after suspending)
entry  get_len("nope"): value 0 (after suspending)
entry  get_len(""): error 22 (inline)
sender get_len | then(n * 2): value 18
sender get_len | then(n * 2): error 22 (EINVAL)
```

One change to the convention fell out of the port: a deferred task returns a handle
to the event loop, which runs it. In v3 the task ran its caller itself.

## Reading

1. **`function` can keep its constructor, but not its factories, and still hide the
   entry at v3's cost in moves, storage and indirect branches.** The factory has to
   take the curried arguments by reference and return a sender that only points at
   things. That is a new idiom, though a small one: `str& key` for `str key`.
2. **A factory written as P4223 publishes them works, at a price.** Two more moves
   of each class argument per call, a frame per call, and 20% to 30% more
   instructions than the borrowing form. It still costs about half of today's
   published version.
3. **Hiding costs more than v3 measured.** Against the visible entry in the same
   binary: one indirect branch and one key move per asynchronous call, as before,
   but 75 instructions on a hit and 126 on a miss (GCC), where v3 had 41 and 46.
   Timings put the hidden entry 13% to 17% behind the visible one with GCC and 19%
   to 31% with Clang. Part is the generated entry not merging with `start`; part is
   unexplained.
4. **Parity with the synchronous call still holds for the visible entry** (within
   6% of its instructions with GCC, 16% with Clang; equal indirect branches;
   timings within 9%). For the hidden entry the claim is weaker than first stated:
   14% to 24% more instructions with GCC on these bodies; GCC times it 14% to 26%
   slower than synchronous and Clang 29% to 37%.
5. **Published code gets cheaper when recompiled against `hfn`** (2056 against 2484
   instructions, with no source change but the alias). This overstates the gain:
   `hfn` lacks features `function` has, and each one could cost something.
6. **A new caller of an old callee pays the old callee's cost once** (1614: about
   the hidden cache plus a published backing store).
7. **An old caller of a new callee gains only if the cursor reaches the callee.**
   Through today's `get_fn` it doesn't, and rooting a stack (about 615
   instructions, from scenarios 3 and 4) eats the gain. Forwarded, the gain is a
   fifth of the instructions.
8. **Of the two ways to forward it, the cursor query is the one that generalizes.**
   Naming the allocator's type needs no new vocabulary and also makes `function`'s
   own frames cheaper (row 7 against the published version), but it shuts out every
   caller without that allocator. The query's null default keeps old callers
   working, at the cost of a run-time test in the adaptor.
9. **`function` designed for this would forward the cursor without being asked,**
   as it already treats the frame allocator specially. Opting in through
   `queries<...>` changes the declared type of every interface function, which is
   the kind of cost the hidden entry was chosen to avoid.
10. **The bridge can't be invisible without help from `connect`.** As built, each
    handle-aware sender either carries a C++26 `connect` (as `hfn` does) or is
    wrapped by hand. The adaptor itself is generic and small, so the remaining
    work is in where it is applied: `std::execution::connect`, or a base class
    every handle-aware sender derives from.
11. **Sharing a strand between the cursor and a frame allocator is sound only
    while nothing else allocates from that strand during the operation.** That is
    `strand_allocator`'s own condition, and concurrency breaks it for both.

## What needs deciding

- Whether the default-null cursor query is the mechanism to propose, and whether
  `function` forwards it always or on request.
- Where the bridge is applied: in `connect`, or by each sender.
- Whether the borrowing factory is the recommended way to write a `function`, or
  only what a compiler emits.

Decided since v1 (Ian, 2026-11-15): a handle-aware operation needn't outlive
`start`; legacy and modern senders are bridged by generic adaptors; `function`
should be designed optimally, cursor included.

## What it doesn't show

- A compiler emitting any of this. Every lowering is hand-written.
- Cancellation or any other part of the environment crossing an entry. The
  continuation record has no stop state. `set_stopped` is forwarded and never
  produced.
- Completion on another thread. Everything runs on one thread; the deferred path
  is an event queue drained by the caller.
- That the handle bounds the machine stack: no loop of awaits, no chain of
  suspended completions, no tail call through a handle.
- Raw per-call cost: each body does 400 to 700 instructions of real work.
- Concurrency (forked strands), slab overflow under load, a deep call tree.
- Timing of the suspended path.
- A C implementation behind `hfn` or `legacy_op`: the C++ sender for it in
  `c_demo.cpp` is hand-written.
- The bridge applied by `connect` itself.
- Anything about MSVC, nvc++ or other architectures.

## Record of what was dropped (v3's numbers, GCC, not re-measured)

| | Entry, `int` | Entry, handle run by a loop only | Entry, handle with fall-through |
|---|---|---|---|
| Hit: instructions / indirect branches | 378 / 6 | 387 / 7 | 385 / 6 |
| Miss: instructions / indirect branches | 728 / 10 | 760 / 12 | 742 / 10 |
| Time, hit / miss (ns) | 21 / 48–49 | 22–23 / 53 | 24–25 / 59–64 |

The handle with the fall-through matched the `int` entry's indirect branches and
cost about 2% more instructions; GCC's fall-through miss timed slower for a reason
that wasn't investigated. Moves and storage were equal across the three. Run by a
loop only, each completion that would have been inline costs one more indirect
branch. With no frame allocator in the environment the published version made 1
`operator new` call on a hit and 2 on a miss.

## Pitfall, carried over

stdexec's `STDEXEC_ASSERT` is live unless `NDEBUG` is defined, and its downcast
asserts with a `dynamic_cast`. Any benchmark of `exec::function` needs `-DNDEBUG`.

## Files

`async_abi.h`, `async_abi.c` (the convention and the async stack, in C);
`common.hpp` (shared types, `get_async_stack`, `strand_env`); `handle.hpp`
(handle-aware receiver and sender, `thunk::start`, `hconnect`, `legacy_op`,
`as_cxx26`, `hfn`); `kv_store.hpp` (the interfaces); `stores.cpp`; `bench.cpp`;
`checks.cpp`; `c_store.h`, `c_store.c`, `c_demo.cpp`; `cg.sh`.

```sh
S=<stdexec>/include
gcc -O2 -c async_abi.c && gcc -std=c11 -O2 -c c_store.c
# counts
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -DCOUNT_MOVES -DCOUNT_HEAP \
    -DASYNC_STATS -isystem $S stores.cpp bench.cpp async_abi.o -o count
ASAN_OPTIONS=detect_leaks=0 ./count     # the driver leaks its stores on purpose
# checks
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -isystem $S stores.cpp checks.cpp \
    async_abi.o -o checks && ASAN_OPTIONS=detect_leaks=0 ./checks
# time
g++ -std=c++20 -O2 -DNDEBUG -isystem $S stores.cpp bench.cpp async_abi.o -o bench
./bench 2000000
# callgrind, every configuration: link with -static, then
./cg.sh ./bench_static      # or: bench loop <configuration 0-16> <hit|miss> <calls>
# C
g++ -std=c++20 -O1 -isystem $S c_demo.cpp c_store.o async_abi.o -o c_demo && ./c_demo
```

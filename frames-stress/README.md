# Stress tests for examples/frames (not in the PR)

Sandbox-written stress tests for the phase-based algorithms in stdexec PR
#2401 (`examples/frames`). Kept here, out of the tree, until it's clear
whether the exploration needs tests; not yet cleaned up to the series' style.

- `stop_test.cpp`: `when_all`'s stop handling (evidence for "Let a phase hold
  something only while it runs"). Every Ending × Entry × Data combination;
  stop never, before `start`, or from another thread while running; the
  receiver deletes the operation inside its completion or afterwards.
  Children: a careful stop-callback leaf, `just_error` and `just` on a
  `static_thread_pool`, and (since 2026-11-07) `steady` children on the pool
  that can't fail, so `when_all` takes its no-stop-source path.
- `fin_test.cpp`: `finally` (evidence for "Add a phase-based finally"). Every
  policy × layout (24 combinations); the receiver copies its result and
  deletes the operation inside the completion (the pattern behind stdexec
  326a1180); a value whose n-th copy or move throws, for every n; final
  senders that succeed, fail, stop, or complete on another thread, including
  a final sender whose own error (copied out of the slot before it ends) has
  throwing copies.
- `assoc_test.cpp`: `associate` (evidence for "Add a phase-based associate").
  Every Ending × Entry × Data combination; work on a `static_thread_pool`
  associated with a `simple_counting_scope` inside `when_all`, joined
  afterwards; a failing child; a closed scope (must complete with
  `set_stopped`); the receiver deletes the operation inside its completion.
- `assoc_lifetime_test.cpp`: `associate`'s associate-data invariant: every
  wrapped sender created from `associate`'s call onwards (moves and copies
  included) is destroyed only while an association is held, or while the
  scope is refusing one, and a refused wrapped sender is destroyed before
  `associate` returns. Five policy combinations, alone, under `repeat_n`
  (non-consuming bind) and under `when_all(then(...))`, and
  `stdexec::associate` as a reference. Single TU, no argument; prints
  `bad 0` per line when clean. Limitation: it counts the scope's live
  associations, not which association covers which wrapped sender, so a
  wrapped sender outliving its own association goes unnoticed while
  another association with the same scope is held.
- `assoc_throw_test.cpp` (2026-11-07): the same invariant when the wrapped
  sender's `connect` throws, alone and under `repeat_n`, for the same five
  policy combinations (`connect_entry` must throw out of `connect`,
  `start_entry` must complete with `set_error`). Prints `bad 0` per line
  when clean.
- `assoc_meas.cpp` (2026-11-07): not a test; prints `associate`'s operation
  size and the wrapped payload's moves and copies under five policies, for
  a plain sender and `then(just(), fn)`, alone and under `repeat_n` x3.
  Used to compare the associate patches below.
- `entry_split_test.cpp` (2026-11-07): compile-time negative test of
  `phased::check_entry`'s split_sender assertion. A two-phase sender whose
  phases both connect `sndr_` enters the second phase first. With
  `-DDATA=split_sender -DENTER_SECOND=1` it must fail with "a part the first
  phase connects on entry isn't kept for another phase"; the controls
  `-DDATA=split_sender -DENTER_SECOND=0` and `-DDATA=stored_sender
  -DENTER_SECOND=1` must compile and print 42. Checked with GCC 13 at PR head
  d51028f: the assertion fires (the only error) and both controls pass.

Each of the first three is split with `-DPART=0..3` because one TU with
every combination exhausts ~8 GB. From the stdexec checkout:

```sh
I="-I include -I examples/frames"
for p in 0 1 2 3; do
  g++-13 -std=c++20 -O1 -g -fsanitize=address,undefined -DPART=$p $I stop_test.cpp -o asan$p && ./asan$p 200
  clang++-18 -std=c++20 -O1 -g -fsanitize=thread -DPART=$p $I stop_test.cpp -o tsan$p && ./tsan$p 50
done
```

(`fin_test.cpp` the same way, without the argument; `assoc_test.cpp` takes an
iteration count, default 500.) With Ubuntu's libc++ under Clang ASan, set
`ASAN_OPTIONS=alloc_dealloc_mismatch=0`.

Results at PR tree with `examples/frames` = f8b66092 (2026-11-03): `stop_test`
40,000 runs clean (32,000 ASan/UBSan, 8,000 TSan); `fin_test` about 117,000
ASan/UBSan and 107,000 TSan runs clean.

Results with the `finally` rework (slot) and `associate` applied on PR head
bc21d38 (2026-11-03; series tip tree as delivered in
`0001-amend-Add-a-phase-based-finally.patch` to `0003-...`): `fin_test`
222,042 ASan/UBSan and 202,842 TSan runs clean; `stop_test` 16,000 ASan/UBSan
(argument 100) and 4,800 TSan (argument 30) runs clean; `assoc_test` 800
ASan/UBSan (argument 100) and 240 TSan (argument 30) iterations clean.

With `0001-amend-Add-a-phase-based-associate.patch` (associate-data) on top
(2026-11-03): `assoc_test` 800 ASan/UBSan and 240 TSan iterations clean;
`assoc_lifetime_test` clean under ASan/UBSan.

With the entry-phase choice (`...associate-entry-phase.patch`) and
`finally` back to one phase per completion signature with `visit_live`
(`...finally-visit_live.patch`) on top (2026-11-03): `fin_test` 222,042
ASan/UBSan and 202,842 TSan runs clean; `stop_test` and `assoc_test` clean
with the same arguments; `assoc_lifetime_test` clean.

With the 2026-11-07 series on PR head 60e2a62 (when_all's phase as its own
join scope, exec's completion-signature helpers, `exception_ptr` only where
something can throw, when_all's stop source only when a child can fail):
`stop_test` (with `steady`) 20,800 ASan/UBSan (argument 100) and 6,240 TSan
(argument 30) runs clean; `fin_test` all 24 combinations clean under
ASan/UBSan and TSan; `assoc_test` clean with arguments 100 and 30;
`assoc_lifetime_test` bad 0 on every line.

With the associate patches of 2026-11-07 evening on PR head d51028f
(`0001-fixup-Add-a-phase-based-associate.patch`, then
`0002-Bind-associate-s-stored-wrapped-sender-in-place-when.patch`, then
`0003-Hand-associate-s-wrapped-sender-straight-to-connect.patch`), at both
0002 and 0003: `assoc_test` clean with arguments 100 (ASan/UBSan) and 30
(TSan) in all four parts; `assoc_lifetime_test` and `assoc_throw_test` bad 0
on every line under GCC 13 and Clang 18 ASan/UBSan.

Negative controls that confirmed the tests can fail:
- removing `deactivate<Phase>()` from `phased::finish` makes `stop_test` hit
  stdexec's `inplace_stop_source` destructor assertion (callback still
  registered) under both endings;
- touching `received<phase_t>()` after `finish` in `finally`'s replay makes
  `fin_test` report heap-use-after-free at that line;
- `assoc_lifetime_test` against `associate.hpp` from the first `associate`
  patch (wrapped sender kept apart from the association) reports `bad 3` on
  every policy line;
- `assoc_lifetime_test` against 0003 with `take()` dropping the association
  instead of moving it into the carried region reports `bad 1` on every
  line without `repeat_n`. The `repeat_n` lines stay clean: under
  `stored_sender` they bind without consuming, so `take()` isn't called,
  and under `split_sender` the child `repeat_n` keeps holds an association
  with the same scope, which the test can't tell apart (see the limitation
  above).

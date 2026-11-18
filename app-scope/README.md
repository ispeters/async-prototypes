# A programme-wide scope joined alongside the app

2026-11-14. An experiment for problem 2.4 of
`synthesis/problem-statements-v1.md`.

## Question

Ian's example of a plausible green-field idiom: create a scope in `main` and run

```cpp
sync_wait(when_all(the_app(), scope.join()));
```

so that the app has programme-wide dynamic concurrency. What does it do today?

## What `app.cpp` does

`the_app` suspends once, onto `sync_wait`'s own loop, and then spawns one piece of
work into the scope. Run with no argument for the idiom as written; with `a` to wrap
the app in `associate(the_app, scope.get_token())`.

## Observed

stdexec `main` 63a0561, GCC 13.3, `-std=c++20 -O1`, x86-64 Linux.

As written:

```text
variant: when_all(the_app, scope.join())
app: running, spawning work into the scope
sync_wait returned; spawned work ran 0 time(s)
```

With `associate` (output unbuffered; killed by a 10-second timeout):

```text
variant: when_all(associate(the_app, token), scope.join())
app: running, spawning work into the scope
```

## Reading

- **As written, the spawned work is silently dropped.** `when_all` starts the join
  while nothing is associated with the scope, so the join completes at once, and
  the app's later `spawn` is refused. Nothing reports it. (Inference about the
  mechanism; the outcome is observed.)
- **With the app associated, which is the natural correction, it hangs.** The app's
  association is released only when its operation state is destroyed, `when_all`
  keeps that state until all its children complete, and the join is one of them.
  This is the direct case recorded on 2026-11-03.
- The form that works today joins *after* the app, not alongside it.

## Limits

- This is stdexec's implementation. Whether both outcomes follow from the standard's
  wording wasn't checked here.
- The first outcome depends on the app suspending before it spawns anything, which
  this test arranges deterministically. An app that spawns before its first
  suspension would keep the scope open.
- Whether the spawned work runs in the second variant wasn't observed.

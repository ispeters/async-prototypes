# Domains and attributes: experiments

Run against stdexec PR #2401's head e72e0fa (main 0064892) and the C++ working
draft at cplusplus/draft 6e43840, with GCC 13.3: `g++ -std=c++20 -I<stdexec>/include`.

| File | What it shows |
|---|---|
| `added_error.cpp` | A domain that rewrites `schedule_from` the way nvexec's stream domain does, adding and sending `set_error_t(gpu_error)`. `schedule(gpu) \| upon_error(h) \| continues_on(cpu)` compiles and delivers the error: `get_completion_signatures<S, Env>` is computed from `transform_sender(S, Env)` ([exec.getcomplsigs]), so the parent's storage already has room for it. |
| `common.hpp` | Inline schedulers `tagged<N>` with domains `dom<N>`, a thread-local recording which one the code runs on, and `report()`: attributes before and after `transform_sender`, and where the receiver was actually completed. |
| `lowered.cpp` | The standard algorithms stdexec lowers through `transform_sender`. |
| `consequences.cpp` | What a parent does with `let_value`'s attributes: `affine`'s hop elision, and which domain `then` is dispatched to. |

## Observations

- [exec.getcomplsigs]: with an environment, a sender's completion signatures
  are those of `transform_sender(sndr, env)`'s result. [exec.snd.general] p5
  lets a user-provided implementation add only error and stopped completions.
  So nvexec's added `cudaError_t` is explicitly allowed, and parents see it.
- P3826R5 (applied to the draft as 79d1055) removed every `get-attrs`,
  including `default-impls`', and replaced them with a semantic rule in
  [exec.snd.general] p3 and p4: completion domains and schedulers follow
  from the algorithm's semantics, the receiver's environment and the
  children's completions. `basic-sender::get_env` still calls
  `impls-for<Tag>::get-attrs`, which no longer exists (editorial leftover).
- [exec.get.compl.sched] and [exec.get.compl.domain]: if connecting a sender
  (which transforms it) produces an operation that completes elsewhere than
  its attributes say, the behaviour is undefined.
- `lowered.cpp`: `continues_on`, `starts_on`, `on` (both forms),
  `stopped_as_optional`, `stopped_as_error` and `bulk` report the same
  completion scheduler and domain before and after lowering, and complete
  there. `when_all_with_variant` gains a completion scheduler after lowering.
- `let_value(just(), [] { return schedule(S1); })` reports a value completion
  scheduler of S0 (the start scheduler, via `just()`) but completes on S1.
  stdexec's `let_*` `__get_attrs` forwards the first child's environment
  under a TODO ("this needs a proper implementation", since 1e21d785, #1770);
  a fuller version is commented out above it.
- `consequences.cpp`: `then(let_value(just(), ->schedule(S7)), f)` is
  dispatched by D0, so D7's customization of `then` isn't chosen, though `f`
  runs on S7. `affine(let_value(...))` doesn't elide its hop: `let_value`
  doesn't forward completion behaviour.

## Inferences, not checked

- Under p4, `let_value(just(), [] { return schedule(S1); })`'s value
  completion scheduler is S1 or absent, never S0, so stdexec's `let_*`
  attributes don't conform. With nvexec, the same shape would run a `then`
  after a `let_value` that ends on a stream through the CPU `then` rather
  than the stream one.
- p3 and p4 describe attributes by "the semantics of the algorithm", so a
  customization that completes somewhere else than the default would make
  the attributes wrong and the program's behaviour undefined: the burden is
  on domain authors, and nothing checks it.
- Not searched: whether stdexec has an issue for the `let_*` attributes
  (GitHub search wasn't available from the sandbox).

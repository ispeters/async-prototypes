// Sender algorithms against handle.hpp, written once for every mode.
//
// A start or completion returns whatever handle it ends with: in TYPED mode
// a typed handle where only one outcome is possible, and the erased handle
// where outcomes branch (either(), common_handle_t). A receiver's
// environment comes from its parent through get_env(); receivers stay one
// pointer, so whatever an environment holds lives in an operation state.
#pragma once
#include "handle.hpp"
#include "env.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <new>
#include <type_traits>

namespace th {
using std::exception_ptr;
using std::same_as;

template <class R, class Sig>
TH_INLINE void* destination_of(R& r) noexcept {
  return r.template destination<Sig>();
}

// ---- just -----------------------------------------------------------------
template <class T>
struct just_sender {
  using value_type = T;
  T value;

  template <class R>
  struct op {
    R rcvr;
    T value;
    op(R r, T v) : rcvr(r), value(std::move(v)) {}
    op(op&&) = delete;

    TH_INLINE auto start() & noexcept {
      return th::set_value(rcvr, std::move(value));
    }
  };

  template <class R>
  op<R> connect(R rcvr) && noexcept(std::is_nothrow_move_constructible_v<T>) {
    return {rcvr, std::move(value)};
  }
};
template <class T>
just_sender<T> just(T v) {
  return {std::move(v)};
}

// ---- just_unless_stopped ------------------------------------------------
// A leaf that asks its receiver's stop token: stopped if stop has been
// requested, else a value. Against a token that can't be stopped it is just.
template <class T>
struct just_unless_stopped_sender {
  using value_type = T;
  T value;

  template <class R>
  struct op {
    R rcvr;
    T value;
    op(R r, T v) : rcvr(r), value(std::move(v)) {}
    op(op&&) = delete;

    TH_INLINE auto start() & noexcept {
      if constexpr (unstoppable<stop_token_of_t<R>>) {
        return th::set_value(rcvr, std::move(value));
      } else {
        return either(
            get_stop_token(th::get_env(rcvr)).stop_requested(),
            [&] { return th::set_stopped(rcvr); },
            [&] { return th::set_value(rcvr, std::move(value)); });
      }
    }
  };

  template <class R>
  op<R> connect(R rcvr) && noexcept(std::is_nothrow_move_constructible_v<T>) {
    return {rcvr, std::move(value)};
  }
};
template <class T>
just_unless_stopped_sender<T> just_unless_stopped(T v) {
  return {std::move(v)};
}

// ---- then -----------------------------------------------------------------
template <class Child, class F, class R>
struct then_op {
  using V = value_of<Child>;
  using U = std::invoke_result_t<F, V>;
  static constexpr bool nothrow = std::is_nothrow_invocable_v<F, V>;

  struct rcvr_t {
    then_op* op;

    template <class Sig>
    void* destination() noexcept {
      if constexpr (same_as<Sig, set_value_t(V)>) {
        return op->value.address();  // filtered: then needs it
      } else {
        return destination_of<R, Sig>(op->rcvr);  // passed through
      }
    }

    template <class Sig>
    TH_INLINE auto complete() noexcept {
      if constexpr (same_as<Sig, set_value_t(V)>) {
        return op->invoke();
      } else {
        return later<Sig>(op->rcvr);
      }
    }

    TH_INLINE env_of_t<R> get_env() const noexcept {
      return th::get_env(op->rcvr);
    }
  };

  F f;
  R rcvr;
  slot<V> value;
  connect_result_t<Child, rcvr_t> child;

  then_op(Child&& c, F fn, R r) noexcept(
      std::is_nothrow_move_constructible_v<F> &&
      noexcept(th::connect(std::declval<Child>(), std::declval<rcvr_t>())))
      : f(std::move(fn)), rcvr(r),
        child(th::connect(std::move(c), rcvr_t{this})) {}
  then_op(then_op&&) = delete;

  TH_INLINE auto start() & noexcept { return th::start(child); }

  TH_INLINE auto invoke() noexcept {
    V& v = value.adopt();
    if constexpr (nothrow) {
      ::new (destination_of<R, set_value_t(U)>(rcvr))
          U(std::invoke(std::move(f), std::move(v)));
      value.reset();  // before completing: the parent may destroy *this
      return later<set_value_t(U)>(rcvr);
    } else {
      bool ok = true;
      try {
        ::new (destination_of<R, set_value_t(U)>(rcvr))
            U(std::invoke(std::move(f), std::move(v)));
      } catch (...) {
        ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
            exception_ptr(std::current_exception());
        ok = false;
      }
      value.reset();
      return either(
          ok, [&] { return later<set_value_t(U)>(rcvr); },
          [&] { return later<set_error_t(exception_ptr)>(rcvr); });
    }
  }
};

template <class Child, class F>
struct then_sender {
  using value_type = std::invoke_result_t<F, value_of<Child>>;
  Child child;
  F f;
  template <class R>
  then_op<Child, F, R> connect(R rcvr) && noexcept(
      std::is_nothrow_constructible_v<then_op<Child, F, R>, Child, F, R>) {
    return {std::move(child), std::move(f), rcvr};
  }
};
template <class Child, class F>
then_sender<Child, F> then(Child c, F f) {
  return {std::move(c), std::move(f)};
}

// ---- let_value ------------------------------------------------------------
template <class Child, class F, class R>
struct let_value_op {
  using V = value_of<Child>;
  using Second = std::invoke_result_t<F, V&>;
  using W = value_of<Second>;

  struct rcvr1_t {  // the first child's
    let_value_op* op;

    template <class Sig>
    void* destination() noexcept {
      if constexpr (same_as<Sig, set_value_t(V)>) {
        return op->value.address();  // kept: the second child may borrow it
      } else {
        return destination_of<R, Sig>(op->rcvr);
      }
    }

    template <class Sig>
    TH_INLINE auto complete() noexcept {
      if constexpr (same_as<Sig, set_value_t(V)>) {
        return op->next();
      } else {
        return later<Sig>(op->rcvr);
      }
    }

    TH_INLINE env_of_t<R> get_env() const noexcept {
      return th::get_env(op->rcvr);
    }
  };

  struct rcvr2_t {  // the second child's: everything passes through
    let_value_op* op;

    template <class Sig>
    void* destination() noexcept {
      return destination_of<R, Sig>(op->rcvr);
    }
    template <class Sig>
    TH_INLINE auto complete() noexcept {
      return later<Sig>(op->rcvr);
    }

    TH_INLINE env_of_t<R> get_env() const noexcept {
      return th::get_env(op->rcvr);
    }
  };

  using op1_t = connect_result_t<Child, rcvr1_t>;
  using op2_t = connect_result_t<Second, rcvr2_t>;
  static constexpr bool nothrow_next =
      std::is_nothrow_invocable_v<F, V&> &&
      noexcept(th::connect(std::declval<Second>(), std::declval<rcvr2_t>()));

  F f;
  R rcvr;
  slot<V> value;  // outlives the second child, which may borrow it
  slot<op1_t> first;
  slot<op2_t> second;

  let_value_op(Child&& c, F fn, R r) : f(std::move(fn)), rcvr(r) {
    first.emplace_from([&] { return th::connect(std::move(c), rcvr1_t{this}); });
  }
  let_value_op(let_value_op&&) = delete;

  TH_INLINE auto start() & noexcept { return th::start(*first); }

  TH_INLINE auto next() noexcept {
    value.adopt();
    first.reset();  // the first child has completed
    auto make = [&] {
      return th::connect(std::invoke(std::move(f), *value), rcvr2_t{this});
    };
    if constexpr (nothrow_next) {
      return start_later(second.emplace_from(make));
    } else {
      op2_t* s = nullptr;
      try {
        s = &second.emplace_from(make);
      } catch (...) {
        ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
            exception_ptr(std::current_exception());
      }
      return either(
          s != nullptr, [&] { return start_later(*s); },
          [&] { return later<set_error_t(exception_ptr)>(rcvr); });
    }
  }
};

template <class Child, class F>
struct let_value_sender {
  using value_type = value_of<std::invoke_result_t<F, value_of<Child>&>>;
  Child child;
  F f;
  template <class R>
  let_value_op<Child, F, R> connect(R rcvr) && {
    return {std::move(child), std::move(f), rcvr};
  }
};
template <class Child, class F>
let_value_sender<Child, F> let_value(Child c, F f) {
  return {std::move(c), std::move(f)};
}

// ---- when_all -------------------------------------------------------------
// D4471's unnamed aggregate of several values, built one element at a time.
template <class A, class B>
struct values {
  union {
    A first;
  };
  union {
    B second;
  };
  values() noexcept {}
  ~values() {
    first.~A();
    second.~B();
  }
};

template <class S1, class S2, class R>
struct when_all_op {
  using V1 = value_of<S1>;
  using V2 = value_of<S2>;
  using Vs = values<V1, V2>;
  enum : int { none, error, stopped };

  template <int I>
  struct rcvr_t {
    when_all_op* op;

    template <class Sig>
    void* destination() noexcept {
      if constexpr (same_as<Sig, set_error_t(exception_ptr)>) {
        return op->errors[I].address();  // staged: both may fail at once
      } else if constexpr (I == 0) {
        return &op->results->first;  // straight into our parent's storage
      } else {
        return &op->results->second;
      }
    }

    template <class Sig>
    TH_INLINE auto complete() noexcept {
      if constexpr (same_as<Sig, set_error_t(exception_ptr)>) {
        op->errors[I].adopt();
        op->fail(I);
      } else if constexpr (same_as<Sig, set_stopped_t()>) {
        op->stop();
      } else {
        op->constructed[I] = true;
      }
      return op->arrive();
    }

    // The children's stop token is when_all's own; the rest is the parent's.
    // (Named, not deduced: a child asks for it while when_all_op is still
    // being defined.)
    TH_INLINE env<prop<get_stop_token_t, inplace_stop_token>, env_of_t<R>>
    get_env() const noexcept {
      return env{prop<get_stop_token_t, inplace_stop_token>{
                     op->stop_source.get_token()},
                 th::get_env(op->rcvr)};
    }
  };

  // Stop requested of the parent is forwarded to the children, unless the
  // parent's token can't be stopped.
  using parent_token = stop_token_of_t<R>;
  static_assert(unstoppable<parent_token> ||
                    same_as<parent_token, inplace_stop_token>,
                "only inplace_stop_token is forwarded");
  struct forward_stop {
    when_all_op* op;
    void operator()() noexcept { op->stop_source.request_stop(); }
  };
  using on_stop_t = inplace_stop_callback<forward_stop>;
  struct no_callback {
    void reset() noexcept {}
  };

  R rcvr;
  inplace_stop_source stop_source;
  [[no_unique_address]] std::conditional_t<unstoppable<parent_token>,
                                           no_callback, slot<on_stop_t>>
      on_stop;
  Vs* results = nullptr;
  bool constructed[2] = {};
  slot<exception_ptr> errors[2];
  int failed = -1;
  std::atomic<int> state{none};
  std::atomic<int> count{2};
  connect_result_t<S1, rcvr_t<0>> child1;
  connect_result_t<S2, rcvr_t<1>> child2;

  when_all_op(S1&& s1, S2&& s2, R r)
      : rcvr(r),
        child1(th::connect(std::move(s1), rcvr_t<0>{this})),
        child2(th::connect(std::move(s2), rcvr_t<1>{this})) {}
  when_all_op(when_all_op&&) = delete;

  TH_INLINE auto start() & noexcept {
    if constexpr (!unstoppable<parent_token>) {
      on_stop.emplace_from([&] {
        return on_stop_t(get_stop_token(th::get_env(rcvr)), forward_stop{this});
      });
    }
    results = ::new (destination_of<R, set_value_t(Vs)>(rcvr)) Vs;
    th::run(th::start(child1));  // a fork: run what it hands back
    return th::start(child2);
  }

  void fail(int i) noexcept {
    if (state.exchange(error) != error) {
      failed = i;
    }
    stop_source.request_stop();
  }
  void stop() noexcept {
    int expected = none;
    state.compare_exchange_strong(expected, stopped);
    stop_source.request_stop();
  }

  using arrive_t = common_handle_t<
      no_operation, decltype(later<set_value_t(Vs)>(std::declval<R&>())),
      decltype(later<set_stopped_t()>(std::declval<R&>())),
      decltype(later<set_error_t(exception_ptr)>(std::declval<R&>()))>;

  TH_INLINE arrive_t arrive() noexcept {
    if (count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
      return no_operation{};
    }
    on_stop.reset();  // before completing: the parent may destroy *this
    if (state.load() == none) {
      return later<set_value_t(Vs)>(rcvr);
    }
    if (constructed[0]) results->first.~V1();  // abandon the results
    if (constructed[1]) results->second.~V2();
    if (state.load() == stopped) {
      return later<set_stopped_t()>(rcvr);
    }
    ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
        exception_ptr(std::move(*errors[failed]));
    return later<set_error_t(exception_ptr)>(rcvr);
  }
};

template <class S1, class S2>
struct when_all_sender {
  using value_type = values<value_of<S1>, value_of<S2>>;
  S1 s1;
  S2 s2;
  template <class R>
  when_all_op<S1, S2, R> connect(R rcvr) && {
    return {std::move(s1), std::move(s2), rcvr};
  }
};
template <class S1, class S2>
when_all_sender<S1, S2> when_all(S1 a, S2 b) {
  return {std::move(a), std::move(b)};
}

// ---- repeat_until ---------------------------------------------------------
// Restarts its child until the child's value is true, then completes with
// set_value_t(). The restart is a deferred start_t() delivered to the child's
// new operation state: no scheduler, no added completion, no stack growth
// unless the mode is EAGER.
template <class Child, class R>
struct repeat_until_op {
  struct rcvr_t {
    repeat_until_op* op;

    template <class Sig>
    void* destination() noexcept {
      if constexpr (same_as<Sig, set_value_t(bool)>) {
        return op->flag.address();
      } else {
        return destination_of<R, Sig>(op->rcvr);
      }
    }

    template <class Sig>
    TH_INLINE auto complete() noexcept {
      if constexpr (same_as<Sig, set_value_t(bool)>) {
        return op->again();
      } else {
        op->child_op.reset();
        return later<Sig>(op->rcvr);
      }
    }

    TH_INLINE env_of_t<R> get_env() const noexcept {
      return th::get_env(op->rcvr);
    }
  };

  using child_op_t = connect_result_t<Child&&, rcvr_t>;
  static constexpr bool nothrow_connect =
      std::is_nothrow_copy_constructible_v<Child> &&
      noexcept(th::connect(std::declval<Child>(), std::declval<rcvr_t>()));

  Child child;
  R rcvr;
  slot<bool> flag;
  slot<child_op_t> child_op;

  repeat_until_op(Child&& c, R r) : child(std::move(c)), rcvr(r) {}
  repeat_until_op(repeat_until_op&&) = delete;

  // Connect a fresh child, then hand back what starting it should be: a
  // plain start from start(), a deferred one from a completion.
  template <class Start>
  TH_INLINE auto connect_then(Start how) noexcept {
    auto make = [&] { return th::connect(Child(child), rcvr_t{this}); };
    if constexpr (nothrow_connect) {
      return how(child_op.emplace_from(make));
    } else {
      child_op_t* c = nullptr;
      try {
        c = &child_op.emplace_from(make);
      } catch (...) {
        ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
            exception_ptr(std::current_exception());
      }
      return either(
          c != nullptr, [&] { return how(*c); },
          [&] { return later<set_error_t(exception_ptr)>(rcvr); });
    }
  }

  TH_INLINE auto start() & noexcept {
    return connect_then([](child_op_t& c) { return th::start(c); });
  }

  // again()'s type has to be named: in EAGER mode the restart nests a call
  // that ends in again() itself, so it can't be deduced. A restart returns
  // what ending does there.
  using done_t = decltype(later<set_value_t()>(std::declval<R&>()));
#if TH_MODE == TH_EAGER
  using restart_t = done_t;
#else
  using restart_t = decltype(start_later(std::declval<child_op_t&>()));
#endif
  using again_t = std::conditional_t<
      nothrow_connect, common_handle_t<done_t, restart_t>,
      common_handle_t<done_t, restart_t,
                      decltype(later<set_error_t(exception_ptr)>(
                          std::declval<R&>()))>>;

#ifdef TH_DEDUCE_AGAIN  // see CMakeLists.txt
  TH_INLINE auto again() noexcept {
#else
  TH_INLINE again_t again() noexcept {
#endif
    bool done = flag.adopt();
    flag.reset();
    child_op.reset();  // the child has completed
    return either(
        done, [&] { return later<set_value_t()>(rcvr); },
        [&] {
          return connect_then([](child_op_t& c) { return start_later(c); });
        });
  }
};

template <class Child>
struct repeat_until_sender {
  using value_type = void;
  Child child;
  template <class R>
  repeat_until_op<Child, R> connect(R rcvr) && {
    return {std::move(child), rcvr};
  }
};
template <class Child>
repeat_until_sender<Child> repeat_until(Child c) {
  return {std::move(c)};
}

// ---- an erased boundary ----------------------------------------------------
// The caller's continuation record: one pointer to a static table with one
// resume function per completion (so no `how`) and one destination function
// per payload. The child sees only erased_receiver<V>, whose completions
// return {table->resume[i], record}: the record is the handle's data.
template <class V>
struct boundary_table {
  erased_handle (*resume[3])(void*) noexcept;  // value, error, stopped
  void* (*destination[2])(void*) noexcept;     // value, error
  inplace_stop_token (*stop_token)(void*) noexcept;  // the one query forwarded
};
template <class V>
struct record {
  boundary_table<V> const* table;
};

// An optimization barrier: the record's address, as if from another TU.
template <class P>
TH_INLINE P opaque(P p) noexcept {
#if defined(__GNUC__)
  asm volatile("" : "+r"(p));
#endif
  return p;
}

template <class V>
struct erased_env {
  record<V>* k;
  TH_INLINE inplace_stop_token query(get_stop_token_t) const noexcept {
    return k->table->stop_token(k);
  }
};

template <class V>
struct erased_receiver {
  record<V>* k;

  TH_INLINE erased_env<V> get_env() const noexcept { return {k}; }

  template <class Sig>
  static constexpr int index = same_as<Sig, set_value_t(V)>                ? 0
                               : same_as<Sig, set_error_t(exception_ptr)> ? 1
                                                                          : 2;
  template <class Sig>
  void* destination() noexcept {
    return k->table->destination[index<Sig>](k);
  }
  template <class Sig>
  TH_INLINE erased_handle complete() noexcept {
    return {k->table->resume[index<Sig>], k};  // returned, not called
  }
};

template <class V, class Child, class R>
struct boundary_op : record<V> {
  using child_op_t = connect_result_t<Child, erased_receiver<V>>;

  template <class Sig>
  static erased_handle resume(void* k) noexcept {
    auto* self = static_cast<boundary_op*>(static_cast<record<V>*>(k));
    return settle(self->rcvr.template complete<Sig>());
  }
  template <class Sig>
  static void* dest(void* k) noexcept {
    auto* self = static_cast<boundary_op*>(static_cast<record<V>*>(k));
    return destination_of<R, Sig>(self->rcvr);
  }
  static inplace_stop_token stop_token_of(void* k) noexcept {
    auto* self = static_cast<boundary_op*>(static_cast<record<V>*>(k));
    if constexpr (unstoppable<stop_token_of_t<R>>) {
      return {};
    } else {
      return get_stop_token(th::get_env(self->rcvr));
    }
  }
  static constexpr boundary_table<V> table_for{
      {&resume<set_value_t(V)>, &resume<set_error_t(exception_ptr)>,
       &resume<set_stopped_t()>},
      {&dest<set_value_t(V)>, &dest<set_error_t(exception_ptr)>},
      &stop_token_of};

  R rcvr;
  child_op_t child;

  boundary_op(Child&& c, R r)
      : record<V>{&table_for}, rcvr(r),
        child(th::connect(std::move(c),
                          erased_receiver<V>{opaque<record<V>*>(this)})) {}
  boundary_op(boundary_op&&) = delete;

  // The callee's start, as if called through an entry in another TU.
  [[gnu::noinline]] static erased_handle start_child(child_op_t* c) noexcept {
    return settle(th::start(*c));
  }
  TH_INLINE erased_handle start() & noexcept {
    return start_child(opaque(&child));
  }
};

template <class Child>
struct boundary_sender {
  using value_type = value_of<Child>;
  Child child;
  template <class R>
  boundary_op<value_type, Child, R> connect(R rcvr) && {
    return {std::move(child), rcvr};
  }
};
template <class Child>
boundary_sender<Child> erase_boundary(Child c) {
  return {std::move(c)};
}

// ---- a root: where the asynchronous world ends --------------------------
template <class V, class Token = never_stop_token>
struct root_state {
  slot<std::conditional_t<std::is_void_v<V>, char, V>> value;
  slot<exception_ptr> error;
  bool stopped = false;
  Token token;  // here, not in the receiver, which stays one pointer
};

template <class V, class Token = never_stop_token>
struct root_rcvr {
  root_state<V, Token>* st;

  TH_INLINE auto get_env() const noexcept {
    if constexpr (unstoppable<Token>) {
      return empty_env{};
    } else {
      return prop<get_stop_token_t, Token>{st->token};
    }
  }

  template <class Sig>
  void* destination() noexcept {
    if constexpr (same_as<Sig, set_error_t(exception_ptr)>) {
      return st->error.address();
    } else {
      return st->value.address();
    }
  }
  template <class Sig>
  TH_INLINE no_operation complete() noexcept {
    if constexpr (same_as<Sig, set_error_t(exception_ptr)>) {
      st->error.adopt();
    } else if constexpr (same_as<Sig, set_stopped_t()>) {
      st->stopped = true;
    } else if constexpr (same_as<Sig, set_value_t()>) {
      st->value.emplace_from([] { return char{}; });
    } else {
      st->value.adopt();
    }
    return {};
  }
};

// Connect s to a root, run it to completion on this thread, then call
// use(state). Everything here completes inline; nothing waits. The root's
// environment has a stop token if one is given.
template <class S, class Use, class Token = never_stop_token>
void run_root(S&& s, Use use, Token token = {}) {
  using V = value_of<S>;
  root_state<V, Token> st;
  st.token = token;
  auto op = th::connect(std::forward<S>(s), root_rcvr<V, Token>{&st});
  th::run(th::start(op));
  use(st);
}

}  // namespace th

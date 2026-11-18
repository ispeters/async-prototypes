// The C++26 columns: algorithms hand-written to the C++26 protocol, against
// stdexec. Regions between [[tony:NAME]] markers appear on the page verbatim.
#pragma once
#include <stdexec/execution.hpp>

#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace c26 {
namespace execution = STDEXEC;
using namespace STDEXEC;
using std::exception_ptr;
using std::same_as;

// Stand-in for computing a child's one value type from its completion
// signatures; both columns use the same name.
template <class S>
using value_of = typename std::remove_cvref_t<S>::value_type;

template <class F>
struct emplace_from {
  F f;
  operator std::invoke_result_t<F&>() && { return f(); }
};
template <class F>
emplace_from(F) -> emplace_from<F>;

template <class V>
using sigs_of = execution::completion_signatures<set_value_t(V), set_error_t(exception_ptr),
                                                 set_stopped_t()>;

// [[tony:just]]
template <class T>
struct just_sender {
  using sender_concept = sender_t;
  using completion_signatures =
      execution::completion_signatures<set_value_t(T)>;

  template <class R>
  struct op {
    using operation_state_concept = operation_state_t;
    R rcvr;
    T value;

    void start() & noexcept {
      execution::set_value(std::move(rcvr), std::move(value));
    }
  };

  T value;

  template <class R>
  op<R> connect(R rcvr) && {
    return {std::move(rcvr), std::move(value)};
  }
};
// [[/tony]]
template <class T>
just_sender(T) -> just_sender<T>;
template <class T>
struct just_sender_v : just_sender<T> {
  using value_type = T;
};
template <class T>
just_sender_v<T> just(T v) {
  return {{std::move(v)}};
}

// a leaf that fails, for the tests
template <class T>
struct just_error_sender {
  using sender_concept = sender_t;
  using value_type = T;
  using completion_signatures = sigs_of<T>;
  template <class R>
  struct op {
    using operation_state_concept = operation_state_t;
    R rcvr;
    void start() & noexcept {
      execution::set_error(std::move(rcvr), std::make_exception_ptr(42));
    }
  };
  template <class R>
  op<R> connect(R rcvr) && {
    return {std::move(rcvr)};
  }
};

// [[tony:then]]
template <class Child, class F, class R>
struct then_op {
  using operation_state_concept = operation_state_t;

  struct rcvr_t {
    using receiver_concept = receiver_t;
    then_op* op;

    template <class... Vs>
    void set_value(Vs&&... vs) && noexcept {
      try {
        execution::set_value(
            std::move(op->rcvr),
            std::invoke(std::move(op->f), std::forward<Vs>(vs)...));
      } catch (...) {
        execution::set_error(std::move(op->rcvr),
                             std::current_exception());
      }
    }

    template <class E>
    void set_error(E&& e) && noexcept {
      execution::set_error(std::move(op->rcvr), std::forward<E>(e));
    }

    void set_stopped() && noexcept {
      execution::set_stopped(std::move(op->rcvr));
    }

    env_of_t<R> get_env() const noexcept {
      return execution::get_env(op->rcvr);
    }
  };

  F f;
  R rcvr;
  connect_result_t<Child, rcvr_t> child;

  then_op(Child&& c, F fn, R r)
      : f(std::move(fn)), rcvr(std::move(r)),
        child(execution::connect(std::move(c), rcvr_t{this})) {}
  then_op(then_op&&) = delete;

  void start() & noexcept { execution::start(child); }
};
// [[/tony]]

template <class Child, class F>
struct then_result_of {
  using type = std::invoke_result_t<F, value_of<Child>>;
};
template <class Child, class F>
  requires requires { typename Child::template then_result<F>; }
struct then_result_of<Child, F> {
  using type = typename Child::template then_result<F>;
};
template <class Child, class F>
struct then_sender {
  using sender_concept = sender_t;
  using value_type = typename then_result_of<Child, F>::type;
  using completion_signatures = sigs_of<value_type>;
  Child child;
  F f;
  template <class R>
  then_op<Child, F, R> connect(R rcvr) && {
    return {std::move(child), std::move(f), std::move(rcvr)};
  }
};

// [[tony:write_env]]
template <class Child, class Env, class R>
struct write_env_op {
  using operation_state_concept = operation_state_t;

  struct rcvr_t {
    using receiver_concept = receiver_t;
    write_env_op* op;

    template <class... Vs>
    void set_value(Vs&&... vs) && noexcept {
      execution::set_value(std::move(op->rcvr), std::forward<Vs>(vs)...);
    }

    template <class E>
    void set_error(E&& e) && noexcept {
      execution::set_error(std::move(op->rcvr), std::forward<E>(e));
    }

    void set_stopped() && noexcept {
      execution::set_stopped(std::move(op->rcvr));
    }

    env<Env, env_of_t<R>> get_env() const noexcept {
      return {op->own_env, execution::get_env(op->rcvr)};
    }
  };

  Env own_env;
  R rcvr;
  connect_result_t<Child, rcvr_t> child;

  write_env_op(Child&& c, Env e, R r)
      : own_env(std::move(e)), rcvr(std::move(r)),
        child(execution::connect(std::move(c), rcvr_t{this})) {}
  write_env_op(write_env_op&&) = delete;

  void start() & noexcept { execution::start(child); }
};
// [[/tony]]

template <class Child, class Env>
struct write_env_sender {
  using sender_concept = sender_t;
  using value_type = value_of<Child>;
  using completion_signatures = sigs_of<value_type>;
  Child child;
  Env env;
  template <class R>
  write_env_op<Child, Env, R> connect(R rcvr) && {
    return {std::move(child), std::move(env), std::move(rcvr)};
  }
};

// [[tony:let_value]]
template <class Child, class F, class R>
struct let_value_op {
  using operation_state_concept = operation_state_t;
  using V = value_of<Child>;
  using Second = std::invoke_result_t<F, V&>;

  struct rcvr1_t {  // the first child's
    using receiver_concept = receiver_t;
    let_value_op* op;

    template <class A>
    void set_value(A&& a) && noexcept {
      op->next(std::forward<A>(a));
    }

    template <class E>
    void set_error(E&& e) && noexcept {
      execution::set_error(std::move(op->rcvr), std::forward<E>(e));
    }

    void set_stopped() && noexcept {
      execution::set_stopped(std::move(op->rcvr));
    }

    env_of_t<R> get_env() const noexcept {
      return execution::get_env(op->rcvr);
    }
  };

  struct rcvr2_t {  // the second child's: everything passes through
    using receiver_concept = receiver_t;
    let_value_op* op;

    template <class... Vs>
    void set_value(Vs&&... vs) && noexcept {
      execution::set_value(std::move(op->rcvr), std::forward<Vs>(vs)...);
    }

    template <class E>
    void set_error(E&& e) && noexcept {
      execution::set_error(std::move(op->rcvr), std::forward<E>(e));
    }

    void set_stopped() && noexcept {
      execution::set_stopped(std::move(op->rcvr));
    }

    env_of_t<R> get_env() const noexcept {
      return execution::get_env(op->rcvr);
    }
  };

  F f;
  R rcvr;
  std::optional<V> value;  // outlives the second child, which may borrow it
  std::variant<connect_result_t<Child, rcvr1_t>,
               connect_result_t<Second, rcvr2_t>>
      ops;

  let_value_op(Child&& c, F fn, R r)
      : f(std::move(fn)), rcvr(std::move(r)),
        ops(std::in_place_index<0>, emplace_from{[&] {
              return execution::connect(std::move(c), rcvr1_t{this});
            }}) {}
  let_value_op(let_value_op&&) = delete;

  void start() & noexcept { execution::start(std::get<0>(ops)); }

  template <class A>
  void next(A&& a) noexcept {
    try {
      value.emplace(std::forward<A>(a));  // a may refer into the first child
      auto& second = ops.template emplace<1>(emplace_from{[&] {
        return execution::connect(std::invoke(std::move(f), *value),
                                  rcvr2_t{this});
      }});  // destroys the first child
      execution::start(second);
    } catch (...) {
      execution::set_error(std::move(rcvr), std::current_exception());
    }
  }
};
// [[/tony]]

template <class Child, class F>
struct let_value_sender {
  using sender_concept = sender_t;
  using value_type = value_of<std::invoke_result_t<F, value_of<Child>&>>;
  using completion_signatures = sigs_of<value_type>;
  Child child;
  F f;
  template <class R>
  let_value_op<Child, F, R> connect(R rcvr) && {
    return {std::move(child), std::move(f), std::move(rcvr)};
  }
};

// [[tony:when_all]]
template <class S1, class S2, class R>
struct when_all_op {
  using operation_state_concept = operation_state_t;
  using V1 = value_of<S1>;
  using V2 = value_of<S2>;
  enum : int { none, error, stopped };

  template <int I>
  struct rcvr_t {
    using receiver_concept = receiver_t;
    when_all_op* op;

    template <class A>
    void set_value(A&& a) && noexcept {
      try {
        std::get<I>(op->results).emplace(std::forward<A>(a));
      } catch (...) {
        op->fail(std::current_exception());
      }
      op->arrive();
    }

    void set_error(exception_ptr e) && noexcept {
      op->fail(std::move(e));
      op->arrive();
    }

    void set_stopped() && noexcept {
      op->stop();
      op->arrive();
    }

    prop<get_stop_token_t, inplace_stop_token> get_env() const noexcept {
      return {get_stop_token, op->stop_source.get_token()};
    }
  };

  R rcvr;
  std::tuple<std::optional<V1>, std::optional<V2>> results;
  exception_ptr first_error;
  std::atomic<int> state{none};
  std::atomic<int> count{2};
  inplace_stop_source stop_source;
  connect_result_t<S1, rcvr_t<0>> child1;
  connect_result_t<S2, rcvr_t<1>> child2;

  when_all_op(S1&& s1, S2&& s2, R r)
      : rcvr(std::move(r)),
        child1(execution::connect(std::move(s1), rcvr_t<0>{this})),
        child2(execution::connect(std::move(s2), rcvr_t<1>{this})) {}

  void start() & noexcept {
    execution::start(child1);
    execution::start(child2);
  }

  void fail(exception_ptr e) noexcept {
    if (state.exchange(error) != error) {
      first_error = std::move(e);
    }
    stop_source.request_stop();
  }

  void stop() noexcept {
    int expected = none;
    state.compare_exchange_strong(expected, stopped);
    stop_source.request_stop();
  }

  void arrive() noexcept {
    if (count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
      return;
    }
    switch (state.load()) {
      case none:
        execution::set_value(std::move(rcvr),
                             std::move(*std::get<0>(results)),
                             std::move(*std::get<1>(results)));
        break;
      case error:
        execution::set_error(std::move(rcvr), std::move(first_error));
        break;
      default:
        execution::set_stopped(std::move(rcvr));
    }
  }
};
// [[/tony]]

template <class S1, class S2>
struct when_all_sender {
  using sender_concept = sender_t;
  using completion_signatures =
      execution::completion_signatures<set_value_t(value_of<S1>, value_of<S2>),
                                       set_error_t(exception_ptr), set_stopped_t()>;
  template <class F>
  using then_result = std::invoke_result_t<F, value_of<S1>, value_of<S2>>;
  S1 s1;
  S2 s2;
  template <class R>
  when_all_op<S1, S2, R> connect(R rcvr) && {
    return {std::move(s1), std::move(s2), std::move(rcvr)};
  }
};

// [[tony:run_loop]]
class run_loop {
  struct task {
    void (*execute)(task*) noexcept;
    task* next = nullptr;
  };

  template <class R>
  struct op : task {
    using operation_state_concept = operation_state_t;
    run_loop* loop;
    R rcvr;

    op(run_loop* l, R r) : task{&run}, loop(l), rcvr(std::move(r)) {}

    void start() & noexcept { loop->push_back(this); }

    static void run(task* t) noexcept {
      auto& self = *static_cast<op*>(t);
      if (get_stop_token(execution::get_env(self.rcvr)).stop_requested()) {
        execution::set_stopped(std::move(self.rcvr));
      } else {
        execution::set_value(std::move(self.rcvr));
      }
    }
  };

 public:
  void run() {
    while (task* t = pop_front()) {
      t->execute(t);
    }
  }
  // [[/tony]]

  void finish() {
    std::lock_guard g(m_);
    finishing_ = true;
    cv_.notify_all();
  }

  struct sender {
    using sender_concept = sender_t;
    using value_type = void;
    using completion_signatures =
        execution::completion_signatures<set_value_t(), set_stopped_t()>;
    template <class F>
    using then_result = std::invoke_result_t<F>;
    run_loop* loop;
    template <class R>
    op<R> connect(R rcvr) const {
      return {loop, std::move(rcvr)};
    }
  };
  struct scheduler {
    using scheduler_concept = scheduler_t;
    run_loop* loop;
    sender schedule() const noexcept { return {loop}; }
    bool operator==(scheduler const&) const = default;
  };
  scheduler get_scheduler() noexcept { return {this}; }

 private:
  void push_back(task* t) {
    std::lock_guard g(m_);
    if (tail_) tail_->next = t; else head_ = t;
    tail_ = t;
    cv_.notify_one();
  }
  task* pop_front() {
    std::unique_lock g(m_);
    cv_.wait(g, [&] { return head_ || finishing_; });
    task* t = head_;
    if (t) {
      head_ = t->next;
      if (!head_) tail_ = nullptr;
    }
    return t;
  }
  std::mutex m_;
  std::condition_variable cv_;
  task* head_ = nullptr;
  task* tail_ = nullptr;
  bool finishing_ = false;
};

// [[tony:sync_wait]]
template <class V>
struct sync_wait_state {
  run_loop loop;
  std::optional<V> value;
  exception_ptr error;
};

template <class V>
struct sync_wait_receiver {
  using receiver_concept = receiver_t;
  sync_wait_state<V>* st;

  template <class A>
  void set_value(A&& a) && noexcept {
    try {
      st->value.emplace(std::forward<A>(a));
    } catch (...) {
      st->error = std::current_exception();
    }
    st->loop.finish();
  }

  void set_error(exception_ptr e) && noexcept {
    st->error = std::move(e);
    st->loop.finish();
  }

  void set_stopped() && noexcept { st->loop.finish(); }

  prop<get_scheduler_t, run_loop::scheduler> get_env() const noexcept {
    return {get_scheduler, st->loop.get_scheduler()};
  }
};

template <class S>
std::optional<value_of<S>> sync_wait(S&& s) {
  using V = value_of<S>;
  sync_wait_state<V> st;
  auto op = execution::connect(std::forward<S>(s), sync_wait_receiver<V>{&st});
  execution::start(op);
  st.loop.run();
  if (st.error) {
    std::rethrow_exception(st.error);
  }
  return std::move(st.value);
}
// [[/tony]]

// constructors with the value types the tests need
template <class Child, class F>
then_sender<Child, F> then(Child c, F f) { return {std::move(c), std::move(f)}; }
template <class Child, class Env>
write_env_sender<Child, Env> write_env(Child c, Env e) { return {std::move(c), std::move(e)}; }
template <class Child, class F>
let_value_sender<Child, F> let_value(Child c, F f) { return {std::move(c), std::move(f)}; }
template <class S1, class S2>
when_all_sender<S1, S2> when_all(S1 a, S2 b) { return {std::move(a), std::move(b)}; }
}  // namespace c26

// The C++29 columns, against the stand-in in x29.hpp. Regions between
// [[tony:NAME]] markers appear on the page verbatim.
#pragma once
#include "x29.hpp"

#include <optional>
#include <variant>

namespace c29 {
using namespace x29;
using std::exception_ptr;
using std::same_as;

template <class V>
using sigs_of = x29::
    completion_signatures<set_value_t(V), set_error_t(exception_ptr), set_stopped_t()>;

// [[tony:just]]
template <class T>
struct just_sender {
    using sender_concept        = sender_t;
    using completion_signatures = execution::completion_signatures<set_value_t(T)>;

    template <class R>
    struct op {
        using operation_state_concept = operation_state_t;
        R rcvr;
        T value;

        operation_handle start() & noexcept {
            return execution::set_value(rcvr, std::move(value));
        }
    };

    T value;

    template <class R>
    op<R> connect(R rcvr) && {
        return {std::move(rcvr), std::move(value)};
    }
};
// [[/tony]]

// [[tony:just_in_place]]
template <class T>
struct just_in_place {
    // ...
    template <class R>
    struct op {
        using operation_state_concept = operation_state_t;
        R rcvr;
        T value;

        operation_handle start() & noexcept {
            ::new (rcvr.template destination<set_value_t(T)>()) T(std::move(value));
            return rcvr.template complete<set_value_t(T)>();
        }
    };
    // ...
    // [[/tony]]
    using sender_concept = sender_t;
    T value;
    template <class R>
    op<R> connect(R rcvr) && {
        return {std::move(rcvr), std::move(value)};
    }
};

template <class T>
struct just_sender_v : just_sender<T> {
    using value_type = T;
};
template <class T>
just_sender_v<T> just(T v) {
    return {{std::move(v)}};
}
template <class T>
struct just_in_place_v : just_in_place<T> {
    using value_type = T;
};
template <class T>
just_in_place_v<T> just_constructing(T v) {
    return {{std::move(v)}};
}

template <class T>
struct just_error_sender {
    using sender_concept = sender_t;
    using value_type     = T;
    template <class R>
    struct op {
        R                rcvr;
        operation_handle start() & noexcept {
            ::new (rcvr.template destination<set_error_t(exception_ptr)>())
                exception_ptr(std::make_exception_ptr(42));
            return rcvr.template complete<set_error_t(exception_ptr)>();
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
    using V                       = value_of<Child>;
    using U                       = std::invoke_result_t<F, V>;

    struct rcvr_t {
        using receiver_concept = receiver_t;
        then_op* op;

        // the child's value, where the child keeps it
        operation_handle set_value(V&& v) noexcept {
            return op->invoke(std::move(v), false);
        }

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<Sig, set_value_t(V)>) {
                return op->value.address(); // filtered: then needs it
            } else {
                return op->rcvr.template destination<Sig>(); // passed through
            }
        }

        template <class Sig>
        operation_handle complete() noexcept {
            if constexpr (same_as<Sig, set_value_t(V)>) {
                return op->invoke(std::move(op->value.adopt()), true);
            } else {
                return op->rcvr.template complete<Sig>();
            }
        }

        env_of_t<R> get_env() const noexcept { return execution::get_env(op->rcvr); }
    };

    F                               f;
    R                               rcvr;
    slot<V>                         value; // the child constructs its value here
    connect_result_t<Child, rcvr_t> child;

    then_op(Child&& c, F fn, R r)
        : f(std::move(fn)),
          rcvr(std::move(r)),
          child(execution::connect(std::move(c), rcvr_t{this})) {}
    then_op(then_op&&) = delete;

    operation_handle start() & noexcept { return execution::start(child); }

    operation_handle invoke(V&& v, bool ours) noexcept {
        bool ok = true;
        try {
            ::new (rcvr.template destination<set_value_t(U)>())
                U(std::invoke(std::move(f), std::move(v)));
        } catch (...) {
            ::new (rcvr.template destination<set_error_t(exception_ptr)>())
                exception_ptr(std::current_exception());
            ok = false;
        }
        if (ours) {
            value.reset(); // before completing: the parent may destroy *this
        }
        return ok ? rcvr.template complete<set_value_t(U)>()
                  : rcvr.template complete<set_error_t(exception_ptr)>();
    }
};
// [[/tony]]

template <class Child, class F>
struct then_sender {
    using sender_concept = sender_t;
    using value_type     = std::invoke_result_t<F, value_of<Child>>;
    Child child;
    F     f;
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

        template <class A>
        operation_handle set_value(A&& a) noexcept {
            return execution::set_value(op->rcvr, std::forward<A>(a));
        }

        template <class Sig>
        void* destination() noexcept {
            return op->rcvr.template destination<Sig>();
        }

        template <class Sig>
        operation_handle complete() noexcept {
            return op->rcvr.template complete<Sig>();
        }

        env<Env, env_of_t<R>> get_env() const noexcept {
            return {op->own_env, execution::get_env(op->rcvr)};
        }
    };

    Env                             own_env;
    R                               rcvr;
    connect_result_t<Child, rcvr_t> child;

    write_env_op(Child&& c, Env e, R r)
        : own_env(std::move(e)),
          rcvr(std::move(r)),
          child(execution::connect(std::move(c), rcvr_t{this})) {}
    write_env_op(write_env_op&&) = delete;

    operation_handle start() & noexcept { return execution::start(child); }
};
// [[/tony]]

template <class Child, class Env>
struct write_env_sender {
    using sender_concept = sender_t;
    using value_type     = value_of<Child>;
    Child child;
    Env   env;
    template <class R>
    write_env_op<Child, Env, R> connect(R rcvr) && {
        return {std::move(child), std::move(env), std::move(rcvr)};
    }
};

// [[tony:let_value]]
template <class Child, class F, class R>
struct let_value_op {
    using operation_state_concept = operation_state_t;
    using V                       = value_of<Child>;
    using Second                  = std::invoke_result_t<F, V&>;

    struct rcvr1_t { // the first child's
        using receiver_concept = receiver_t;
        let_value_op* op;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<Sig, set_value_t(V)>) {
                return op->value.address(); // kept: the second child may borrow it
            } else {
                return op->rcvr.template destination<Sig>();
            }
        }

        template <class Sig>
        operation_handle complete() noexcept {
            if constexpr (same_as<Sig, set_value_t(V)>) {
                op->value.adopt();
                return op->next();
            } else {
                return op->rcvr.template complete<Sig>();
            }
        }

        env_of_t<R> get_env() const noexcept { return execution::get_env(op->rcvr); }
    };

    struct rcvr2_t { // the second child's: everything passes through
        using receiver_concept = receiver_t;
        let_value_op* op;

        template <class A>
        operation_handle set_value(A&& a) noexcept {
            return execution::set_value(op->rcvr, std::forward<A>(a));
        }

        template <class Sig>
        void* destination() noexcept {
            return op->rcvr.template destination<Sig>();
        }

        template <class Sig>
        operation_handle complete() noexcept {
            return op->rcvr.template complete<Sig>();
        }

        env_of_t<R> get_env() const noexcept { return execution::get_env(op->rcvr); }
    };

    F       f;
    R       rcvr;
    slot<V> value; // outlives the second child, which may borrow it
    std::variant<connect_result_t<Child, rcvr1_t>, connect_result_t<Second, rcvr2_t>> ops;

    let_value_op(Child&& c, F fn, R r)
        : f(std::move(fn)),
          rcvr(std::move(r)),
          ops(std::in_place_index<0>, emplace_from{[&] {
                  return execution::connect(std::move(c), rcvr1_t{this});
              }}) {}
    let_value_op(let_value_op&&) = delete;

    operation_handle start() & noexcept { return execution::start(std::get<0>(ops)); }

    operation_handle next() noexcept {
        try {
            // destroys the first child
            auto& second = ops.template emplace<1>(emplace_from{[&] {
                return execution::connect(std::invoke(std::move(f), *value),
                                          rcvr2_t{this});
            }});
            return execution::start(second);
        } catch (...) {
            ::new (rcvr.template destination<set_error_t(exception_ptr)>())
                exception_ptr(std::current_exception());
            return rcvr.template complete<set_error_t(exception_ptr)>();
        }
    }
};
// [[/tony]]

template <class Child, class F>
struct let_value_sender {
    using sender_concept = sender_t;
    using value_type     = value_of<std::invoke_result_t<F, value_of<Child>&>>;
    Child child;
    F     f;
    template <class R>
    let_value_op<Child, F, R> connect(R rcvr) && {
        return {std::move(child), std::move(f), std::move(rcvr)};
    }
};

// [[tony:when_all]]
template <class S1, class S2, class R>
struct when_all_op {
    using operation_state_concept = operation_state_t;
    using V1                      = value_of<S1>;
    using V2                      = value_of<S2>;
    using Vs                      = values<V1, V2>; // D4471's unnamed aggregate
    enum : int { none, error, stopped };

    template <int I>
    struct rcvr_t {
        using receiver_concept = receiver_t;
        when_all_op* op;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<Sig, set_error_t(exception_ptr)>) {
                return op->errors[I].address(); // staged: both may fail at once
            } else if constexpr (I == 0) {
                return &op->results->first; // straight into our parent's storage
            } else {
                return &op->results->second;
            }
        }

        template <class Sig>
        operation_handle complete() noexcept {
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

        prop<get_stop_token_t, inplace_stop_token> get_env() const noexcept {
            return {get_stop_token, op->stop_source.get_token()};
        }
    };

    R                               rcvr;
    Vs*                             results        = nullptr; // in our parent's storage
    bool                            constructed[2] = {};
    slot<exception_ptr>             errors[2];
    int                             failed = -1;
    std::atomic<int>                state{none};
    std::atomic<int>                count{2};
    inplace_stop_source             stop_source;
    connect_result_t<S1, rcvr_t<0>> child1;
    connect_result_t<S2, rcvr_t<1>> child2;

    when_all_op(S1&& s1, S2&& s2, R r)
        : rcvr(std::move(r)),
          child1(execution::connect(std::move(s1), rcvr_t<0>{this})),
          child2(execution::connect(std::move(s2), rcvr_t<1>{this})) {}

    operation_handle start() & noexcept {
        // asked once, up front; each child gets its element
        results = ::new (rcvr.template destination<set_value_t(Vs)>()) Vs;
        execution::run(execution::start(child1)); // a fork: run what it hands back
        return execution::start(child2);
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

    operation_handle arrive() noexcept {
        if (count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return {};
        }
        if (state.load() == none) {
            return rcvr.template complete<set_value_t(Vs)>();
        }
        // abandon the results
        if (constructed[0]) {
            results->first.~V1();
        }
        if (constructed[1]) {
            results->second.~V2();
        }
        if (state.load() == stopped) {
            return rcvr.template complete<set_stopped_t()>();
        }
        ::new (rcvr.template destination<set_error_t(exception_ptr)>())
            exception_ptr(std::move(*errors[failed]));
        return rcvr.template complete<set_error_t(exception_ptr)>();
    }
};
// [[/tony]]

template <class S1, class S2>
struct when_all_sender {
    using sender_concept = sender_t;
    using value_type     = values<value_of<S1>, value_of<S2>>;
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
        operation_handle (*execute)(task*) noexcept;
        task* next = nullptr;
    };

    template <class R>
    struct op : task {
        using operation_state_concept = operation_state_t;
        run_loop* loop;
        R         rcvr;

        op(run_loop* l, R r) : task{&run}, loop(l), rcvr(std::move(r)) {}

        operation_handle start() & noexcept {
            loop->push_back(this);
            return {}; // nothing is ready yet
        }

        static operation_handle run(task* t) noexcept {
            auto& self = *static_cast<op*>(t);
            if (get_stop_token(execution::get_env(self.rcvr)).stop_requested()) {
                return self.rcvr.template complete<set_stopped_t()>();
            } else {
                return self.rcvr.template complete<set_value_t()>();
            }
        }
    };

  public:
    void run(operation_handle h = {}) {
        execution::run(h); // whatever the caller's start handed back
        while (task* t = pop_front()) {
            execution::run(t->execute(t));
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
        run_loop* loop;
        template <class R>
        op<R> connect(R rcvr) const {
            return {loop, std::move(rcvr)};
        }
    };
    struct scheduler {
        run_loop* loop;
        sender    schedule() const noexcept { return {loop}; }
        bool      operator==(const scheduler&) const = default;
    };
    scheduler get_scheduler() noexcept { return {this}; }

  private:
    void push_back(task* t) {
        std::lock_guard g(m_);
        if (tail_) {
            tail_->next = t;
        } else {
            head_ = t;
        }
        tail_ = t;
        cv_.notify_one();
    }
    task* pop_front() {
        std::unique_lock g(m_);
        cv_.wait(g, [&] { return head_ || finishing_; });
        task* t = head_;
        if (t) {
            head_ = t->next;
            if (!head_) {
                tail_ = nullptr;
            }
        }
        return t;
    }
    std::mutex              m_;
    std::condition_variable cv_;
    task*                   head_      = nullptr;
    task*                   tail_      = nullptr;
    bool                    finishing_ = false;
};

// a then for value-less children, for the run_loop test only
template <class F, class R>
struct then_void_op {
    struct rcvr_t {
        then_void_op* op;
        template <class Sig>
        void* destination() noexcept {
            return op->rcvr.template destination<Sig>();
        }
        template <class Sig>
        operation_handle complete() noexcept {
            if constexpr (same_as<Sig, set_value_t()>) {
                using U = std::invoke_result_t<F>;
                ::new (op->rcvr.template destination<set_value_t(U)>()) U(op->f());
                return op->rcvr.template complete<set_value_t(U)>();
            } else {
                return op->rcvr.template complete<Sig>();
            }
        }
        env_of_t<R> get_env() const noexcept { return execution::get_env(op->rcvr); }
    };
    F                                          f;
    R                                          rcvr;
    connect_result_t<run_loop::sender, rcvr_t> child;
    then_void_op(run_loop::sender s, F fn, R r)
        : f(std::move(fn)), rcvr(std::move(r)), child(s.connect(rcvr_t{this})) {}
    operation_handle start() & noexcept { return execution::start(child); }
};

// [[tony:sync_wait]]
template <class V>
struct sync_wait_state {
    run_loop            loop;
    slot<V>             value;
    slot<exception_ptr> error;
};

template <class V>
struct sync_wait_receiver {
    using receiver_concept = receiver_t;
    sync_wait_state<V>* st;

    template <class Sig>
    void* destination() noexcept {
        if constexpr (same_as<Sig, set_value_t(V)>) {
            return st->value.address();
        } else {
            return st->error.address();
        }
    }

    template <class Sig>
    operation_handle complete() noexcept {
        if constexpr (same_as<Sig, set_value_t(V)>) {
            st->value.adopt();
        } else if constexpr (!same_as<Sig, set_stopped_t()>) {
            st->error.adopt();
        }
        st->loop.finish();
        return {}; // nothing more to run here
    }

    prop<get_scheduler_t, run_loop::scheduler> get_env() const noexcept {
        return {get_scheduler, st->loop.get_scheduler()};
    }
};

template <class S>
std::optional<value_of<S>> sync_wait(S&& s) {
    using V = value_of<S>;
    sync_wait_state<V> st;
    auto op = execution::connect(std::forward<S>(s), sync_wait_receiver<V>{&st});
    st.loop.run(execution::start(op));
    if (st.error.has_value()) {
        std::rethrow_exception(*st.error);
    }
    if (!st.value.has_value()) {
        return std::nullopt; // stopped
    }
    return std::optional<V>(std::move(*st.value));
}
// [[/tony]]

template <class Child, class F>
then_sender<Child, F> then(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class Child, class Env>
write_env_sender<Child, Env> write_env(Child c, Env e) {
    return {std::move(c), std::move(e)};
}
template <class Child, class F>
let_value_sender<Child, F> let_value(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class S1, class S2>
when_all_sender<S1, S2> when_all(S1 a, S2 b) {
    return {std::move(a), std::move(b)};
}
} // namespace c29

// A library-only stand-in for the C++29 sender protocol the Tony tables
// assume, built on abi4a.h. Every name in namespace x29 is a strawman.
#pragma once
#include "abi4a.h"

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>

namespace x29 {
struct set_value_t {};
struct set_error_t {};
struct set_stopped_t {};
inline constexpr set_value_t set_value_tag{};
struct sender_t {};
struct receiver_t {};
struct operation_state_t {};
template <class... Sigs>
struct completion_signatures {};
struct empty_env {};

// What start and every completion return: the next thing to run (D4481).
using operation_handle = async_next;
using continuation     = async_cont;

template <std::size_t N>
struct completion_table { // the layout of ASYNC_TABLE(N)
    async_resume_fn resume;
    async_dest_fn   destination[N];
};

inline void run(operation_handle h) noexcept { async_run(h); }

// set_value(rcvr, a): the sender's choice when its value already exists and
// outlives the call. A receiver that can use the value where it is provides a
// set_value member; any other receiver gets the construct/complete pair.
template <class R, class A> // stand-in: one value
operation_handle set_value(R& rcvr, A&& a) noexcept {
    if constexpr (requires { rcvr.set_value(std::forward<A>(a)); }) {
        return rcvr.set_value(std::forward<A>(a));
    } else {
        using T = std::decay_t<A>;
        ::new (rcvr.template destination<set_value_t(T)>()) T(std::forward<A>(a));
        return rcvr.template complete<set_value_t(T)>();
    }
}

template <class Op>
operation_handle start(Op& op) noexcept {
    return op.start();
}
template <class S, class R>
auto connect(S&& s, R r) {
    return std::forward<S>(s).connect(std::move(r));
}
template <class S, class R>
using connect_result_t = decltype(x29::connect(std::declval<S>(), std::declval<R>()));
template <class R>
decltype(auto) get_env(const R& r) noexcept {
    return r.get_env();
}
template <class R>
using env_of_t = decltype(x29::get_env(std::declval<const R&>()));

// Stand-in for computing a child's one value type from its completion
// signatures; both columns use the same name.
template <class S>
using value_of = typename std::remove_cvref_t<S>::value_type;

// Stand-in for D4471's unnamed aggregate of several values, built one element
// at a time. C++ has no such type; see the when_all section.
template <class A, class B>
struct values {
    union {
        A first;
    };
    union {
        B second;
    };
    values() noexcept {} // constructs no element
    ~values() {
        first.~A();
        second.~B();
    }
};

// Queries and environments, as in C++26 (stand-ins).
struct never_stop_token;
struct get_stop_token_t {
    template <class E>
    auto operator()(const E& e) const noexcept;
};
inline constexpr get_stop_token_t get_stop_token{};
struct get_scheduler_t {};
inline constexpr get_scheduler_t get_scheduler{};

template <class Q, class V>
struct prop {
    Q q;
    V v;
    V query(Q) const noexcept { return v; }
};

template <class E1, class E2>
struct env { // the join of two environments, the first winning
    E1 first;
    E2 second;
    template <class Q>
    auto query(Q q) const noexcept
        requires requires { first.query(q); } || requires { second.query(q); }
    {
        if constexpr (requires { first.query(q); })
            return first.query(q);
        else
            return second.query(q);
    }
};
template <class E1, class E2>
env(E1, E2) -> env<E1, E2>;

struct inplace_stop_token {
    const std::atomic<bool>* flag = nullptr;
    bool stop_requested() const noexcept { return flag && flag->load(); }
};
struct inplace_stop_source {
    std::atomic<bool>  flag{false};
    void               request_stop() noexcept { flag.store(true); }
    inplace_stop_token get_token() const noexcept { return {&flag}; }
};
struct never_stop_token {
    static constexpr bool stop_requested() noexcept { return false; }
};
template <class E>
auto get_stop_token_t::operator()(const E& e) const noexcept {
    if constexpr (requires { e.query(*this); })
        return e.query(*this);
    else
        return never_stop_token{};
}

// Storage for a T that someone else constructs: what a destination points
// at. std::optional can't hand out the address of storage it hasn't
// constructed into, so the design needs a type like this (stdexec has
// __manual_lifetime, which doesn't track whether it holds an object).
template <class T>
class slot {
  public:
    slot() noexcept {}
    ~slot() { reset(); }
    void* address() noexcept { return &value_; }
    T&    adopt() noexcept { // an object was constructed at address()
        engaged_ = true;
        return value_;
    }
    T&   operator*() noexcept { return value_; }
    bool has_value() const noexcept { return engaged_; }
    void reset() noexcept {
        if (engaged_) {
            value_.~T();
            engaged_ = false;
        }
    }

  private:
    union {
        T value_;
    };
    bool engaged_ = false;
};

// A helper both columns use to emplace an immovable object from a function's
// result (stdexec has the same, as __emplace_from).
template <class F>
struct emplace_from {
    F f;
      operator std::invoke_result_t<F&>() && { return f(); }
};
template <class F>
emplace_from(F) -> emplace_from<F>;

} // namespace x29

namespace execution = x29;

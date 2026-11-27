// Environments and stop tokens, as small stand-ins for C++26's.
//
// get_env(r) is r.get_env() if r has one, else an empty environment.
// get_stop_token(e) is e's answer to the query, else never_stop_token, whose
// stop_possible() is a constant false, so code that asks can be compiled
// away for receivers that can't be stopped.
//
// Stop tokens, sources and callbacks are stdexec's.
#pragma once
#include "handle.hpp"

#include <stdexec/stop_token.hpp>

#include <atomic>
#include <utility>

namespace th {

struct empty_env {};

template <class R>
TH_INLINE auto get_env(const R& r) noexcept {
    if constexpr (requires { r.get_env(); }) {
        return r.get_env();
    } else {
        return empty_env{};
    }
}
template <class R>
using env_of_t = decltype(th::get_env(std::declval<const R&>()));

template <class Q, class V>
struct prop {
    V           value;
    TH_INLINE V query(Q) const noexcept { return value; }
};

template <class E1, class E2>
struct env { // the join of two environments, the first winning
    E1 first;
    E2 second;
    template <class Q>
        requires requires(E1 const& a, E2 const& b, Q q) { a.query(q); } ||
                 requires(E2 const& b, Q q) { b.query(q); }
    TH_INLINE auto query(Q q) const noexcept {
        if constexpr (requires { first.query(q); }) {
            return first.query(q);
        } else {
            return second.query(q);
        }
    }
};
template <class E1, class E2>
env(E1, E2) -> env<E1, E2>;

using stdexec::inplace_stop_source;
using stdexec::inplace_stop_token;
using stdexec::never_stop_token;
template <class F>
using inplace_stop_callback = stdexec::inplace_stop_callback<F>;

struct get_stop_token_t {
    template <class E>
    TH_INLINE auto operator()(const E& e) const noexcept {
        if constexpr (requires { e.query(*this); }) {
            return e.query(*this);
        } else {
            return never_stop_token{};
        }
    }
};
inline constexpr get_stop_token_t get_stop_token{};

template <class R>
using stop_token_of_t = decltype(get_stop_token(get_env(std::declval<const R&>())));

template <class Token>
inline constexpr bool unstoppable = stdexec::unstoppable_token<Token>;

} // namespace th

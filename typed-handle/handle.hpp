// Typed and erased operation handles.
//
// Every transfer of control in the protocol delivers a signal to a target:
// an operation state receives start_t(), and a receiver receives one of its
// completion signatures. A handle is such a delivery, deferred:
//
//   operation_handle<T, Sig>   typed: delivers Sig to a T; the type names
//                              the code to run, so a chain of typed handles
//                              can be followed, and inlined, at compile time
//   operation_handle<>         erased: two words, {function, data}; the
//                              function delivers one signal to one target
//   no_operation               typed: there is nothing to run
//   choice<Hs...>              typed (VARIANT mode only): one of several
//                              typed handles, with an index saying which
//
// Every typed handle holds one pointer to the operation state whose code it
// runs, and operation states are immovable. For start_t() that is the
// operation being started. For a completion it is the receiver's parent, not
// the receiver: a receiver needn't be copyable, and it lives in the child,
// which may be gone by the time the handle runs. So a receiver type R names
// its parent's type as R::operation_type, a receiver hands out a pointer to
// its parent through operation_state(), and running the handle calls the
// static R::complete<Sig>(op). Completions carry no values: a value is
// constructed at the receiver's destination before the completion is
// delivered.
//
// Where two different typed handles meet (a branch that may complete either
// way), the common type is the erased handle, or in VARIANT mode a choice of
// every typed handle that can come back. Following a chain stops at an
// erased handle, at no_operation, or at a type already seen (a cycle). A
// typed handle whose own chain can come back to it heads a loop of known
// types, which runs as a loop. run() loops over erased handles until there
// is nothing left.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(__GNUC__)
    #define TH_INLINE inline __attribute__((always_inline))
#else
    #define TH_INLINE inline
#endif

// How deferred deliveries are made (see later() below). Each program is
// built with one mode.
#define TH_EAGER 0   // deliver at once, nesting calls, as C++26 does
#define TH_ERASED 1  // defer every delivery as an erased handle
#define TH_TYPED 2   // defer every delivery as a typed handle
#define TH_VARIANT 3 // as TYPED, but a branch is a choice of typed handles
#ifndef TH_MODE
    #define TH_MODE TH_TYPED
#endif

#include <exec/completion_signatures.hpp>

namespace th {

struct start_t {}; // the signal an operation state receives
// The completion tags and lists of signatures are stdexec's.
using stdexec::set_error_t;
using stdexec::set_stopped_t;
using stdexec::set_value_t;
template <class... Sigs>
using completion_signatures = stdexec::completion_signatures<Sigs...>;

template <class T = void, class Sig = void>
class operation_handle;

// The erased handle: returned in two registers on x86-64 and AArch64.
template <>
class operation_handle<void, void> {
  public:
    using fn_t = operation_handle (*)(void*) noexcept;

    constexpr operation_handle() noexcept = default;
    constexpr operation_handle(fn_t fn, void* data) noexcept : fn_(fn), data_(data) {}

    explicit         operator bool() const noexcept { return fn_ != nullptr; }
    operation_handle operator()() const noexcept { return fn_(data_); }

    friend bool operator==(operation_handle, operation_handle) = default;

  private:
    fn_t  fn_   = nullptr;
    void* data_ = nullptr;
};

using erased_handle = operation_handle<>;

struct no_operation {
    constexpr operator erased_handle() const noexcept { return {}; }
};

// What a typed handle points at: the operation state that start_t() starts,
// or the parent of a receiver that receives a completion.
template <class T, class Sig>
struct target_of {
    using type = typename T::operation_type*;
};
template <class T>
struct target_of<T, start_t()> {
    using type = T*;
};

template <class H>
inline constexpr bool is_erased = std::same_as<H, erased_handle>;
template <class H>
inline constexpr bool is_none = std::same_as<H, no_operation>;

template <class T, class Sig>
TH_INLINE erased_handle settle(operation_handle<T, Sig> h) noexcept;

// The typed handle.
template <class T, class Sig>
class operation_handle {
  public:
    using target_type = typename target_of<T, Sig>::type;

    TH_INLINE explicit operation_handle(target_type t) noexcept : t_(t) {}

    TH_INLINE decltype(auto) operator()() const noexcept {
        if constexpr (std::same_as<Sig, start_t()>) {
            return t_->start();
        } else {
            return T::template complete<Sig>(*t_);
        }
    }

    // Erasure: {a function for this T and Sig, the target as data}. The
    // target is always one pointer, so every typed handle can be erased.
    operator erased_handle() const noexcept { return {&thunk, static_cast<void*>(t_)}; }

  private:
    static erased_handle thunk(void* data) noexcept {
        return settle(operation_handle(static_cast<target_type>(data)));
    }

    target_type t_;
};

// Lookups over a pack go through an array rather than a fold expression or a
// recursion, so a long pack meets neither Clang's limit on the operands of a
// fold (256 by default) nor the limit on the depth of instantiation.
template <bool... Bs>
consteval bool all_of() noexcept {
    constexpr bool b[] = {Bs..., true};
    for (bool x : b) {
        if (!x) {
            return false;
        }
    }
    return true;
}
template <bool... Bs>
consteval bool any_of() noexcept {
    constexpr bool b[] = {Bs..., false};
    for (bool x : b) {
        if (x) {
            return true;
        }
    }
    return false;
}
// Where H is in Hs, or sizeof...(Hs) if it isn't there.
template <class H, class... Hs>
consteval std::size_t pack_index() noexcept {
    constexpr bool same[] = {std::same_as<H, Hs>..., true};
    std::size_t    i      = 0;
    while (!same[i]) {
        ++i;
    }
    return i;
}
template <class H, class... Hs>
inline constexpr bool in_pack = pack_index<H, Hs...>() < sizeof...(Hs);

template <class... Hs>
class choice;
template <class H>
inline constexpr bool is_choice = false;
template <class... Hs>
inline constexpr bool is_choice<choice<Hs...>> = true;

template <class... Seen, class H>
TH_INLINE erased_handle settle_(H h) noexcept;

// The smallest unsigned type that can index N alternatives.
template <std::size_t N>
using choice_index_t = std::conditional_t<
    (N <= std::size_t(std::numeric_limits<unsigned char>::max()) + 1),
    unsigned char,
    std::conditional_t<(N <= std::size_t(std::numeric_limits<unsigned short>::max()) + 1),
                       unsigned short,
                       std::size_t>>;

// One of several typed handles (or no_operation). Following it branches on
// the index, and each branch carries on along its own chain at compile time.
// There is no limit on the number of alternatives beyond the compiler's own.
template <class... Hs>
class choice {
    static_assert(sizeof...(Hs) >= 2);
    static_assert(all_of<std::is_trivially_copyable_v<Hs>...>());

  public:
    template <class H>
        requires in_pack<H, Hs...>
    TH_INLINE choice(H h) noexcept : index_(index_of<H>) {
        ::new (storage_) H(h);
    }

    template <class... Gs> // from a choice of fewer alternatives
    TH_INLINE choice(choice<Gs...> c) noexcept
        : choice(c.visit([](auto h) noexcept { return choice(h); })) {}

    template <class F>
    TH_INLINE decltype(auto) visit(F f) const noexcept {
        return visit_<0, sizeof...(Hs)>(f);
    }

    operator erased_handle() const noexcept {
        return visit([](auto h) noexcept -> erased_handle { return h; });
    }

  private:
    using index_t = choice_index_t<sizeof...(Hs)>;

    template <class H>
    static constexpr index_t index_of = index_t(pack_index<H, Hs...>());

    // Calls f with the alternative whose index is in [Lo, Hi), halving the
    // range at each step, so the depth of instantiation grows with the
    // logarithm of the number of alternatives, not with the number itself.
    template <std::size_t Lo, std::size_t Hi, class F>
    TH_INLINE decltype(auto) visit_(F& f) const noexcept {
        if constexpr (Hi - Lo == 1) {
            using H = std::tuple_element_t<Lo, std::tuple<Hs...>>;
            return f(*std::launder(reinterpret_cast<const H*>(storage_)));
        } else {
            constexpr std::size_t mid = Lo + (Hi - Lo) / 2;
            if (index_ < mid) {
                return visit_<Lo, mid>(f);
            }
            return visit_<mid, Hi>(f);
        }
    }

    // A tagged union by hand: GCC 13 optimizes std::variant less well here.
    alignas(Hs...) unsigned char storage_[std::max({sizeof(Hs)...})];
    index_t index_;
};

// ---- Cycles of known types ------------------------------------------------
// What running a handle can hand back, as types.
template <class... Hs>
struct hlist {};
template <class H>
struct successors {
    using type = hlist<decltype(std::declval<const H&>()())>;
};
template <>
struct successors<erased_handle> {
    using type = hlist<>;
};
template <>
struct successors<no_operation> {
    using type = hlist<>;
};
template <class... Hs>
struct successors<choice<Hs...>> {
    using type = hlist<Hs...>;
};

// Whether following from H can come back to Target without passing through
// a type in Visited.
template <class Target, class Visited, class H>
struct reaches;
template <class Target, class... Visited, class H>
struct reaches<Target, hlist<Visited...>, H> {
    template <class S>
    static constexpr bool via() noexcept {
        if constexpr (std::same_as<S, Target>) {
            return true;
        } else if constexpr (std::same_as<S, H> || (std::same_as<S, Visited> || ...)) {
            return false;
        } else {
            return reaches<Target, hlist<Visited..., H>, S>::value;
        }
    }
    template <class... Ss>
    static constexpr bool any(hlist<Ss...>) noexcept {
        return any_of<via<Ss>()...>();
    }
    static constexpr bool value = any(typename successors<H>::type{});
};

// A loop's head: a typed handle whose own chain can come back to it.
template <class H>
inline constexpr bool is_loop_head = false;
template <class T, class Sig>
inline constexpr bool is_loop_head<operation_handle<T, Sig>> =
    reaches<operation_handle<T, Sig>, hlist<>, operation_handle<T, Sig>>::value;

// One trip round a loop: back at the head, or out of the loop.
template <class Head>
struct loop_step {
    TH_INLINE explicit loop_step(Head h) noexcept : again(true), next(h) {}
    TH_INLINE explicit loop_step(erased_handle e) noexcept : again(false), exit(e) {}
    bool again;
    union {
        Head          next;
        erased_handle exit;
    };
};

template <class Head, class... Seen, class X>
TH_INLINE loop_step<Head> step_(X x) noexcept {
    if constexpr (std::same_as<X, Head>) {
        return loop_step<Head>(x);
    } else if constexpr (is_erased<X>) {
        return loop_step<Head>(x);
    } else if constexpr (is_none<X>) {
        return loop_step<Head>(erased_handle{});
    } else if constexpr ((std::same_as<X, Seen> || ...)) {
        return loop_step<Head>(erased_handle(x)); // another cycle: leave
    } else if constexpr (is_choice<X>) {
        return x.visit([](auto alt) noexcept -> loop_step<Head> {
            return step_<Head, Seen..., X>(alt);
        });
    } else {
        return step_<Head, Seen..., X>(x());
    }
}

// Run a loop of known types as a loop: each trip follows the chain from the
// head at compile time, and only leaving the loop is erased.
template <class... Seen, class Head>
TH_INLINE erased_handle loop_(Head h) noexcept {
    for (;;) {
        loop_step<Head> s = step_<Head, Seen..., Head>(h());
        if (!s.again) {
            return s.exit;
        }
        h = s.next;
    }
}

// Follow a chain of typed handles at compile time until it reaches an erased
// handle, nothing, a loop's head, or a type already seen. The depth of this
// recursion is bounded by the number of distinct handle types, not by how
// often the chain runs, and it is inlined.
template <class... Seen, class H>
TH_INLINE erased_handle settle_(H h) noexcept {
    if constexpr (is_erased<H>) {
        return h;
    } else if constexpr (is_none<H>) {
        return {};
    } else if constexpr ((std::same_as<H, Seen> || ...)) {
        return h; // a cycle not entered at its head: hand it on, erased
    } else if constexpr (is_loop_head<H>) {
        return loop_<Seen...>(h);
    } else if constexpr (is_choice<H>) {
        return h.visit(
            [](auto alt) noexcept -> erased_handle { return settle_<Seen..., H>(alt); });
    } else {
        return settle_<Seen..., H>(h());
    }
}

template <class T, class Sig>
TH_INLINE erased_handle settle(operation_handle<T, Sig> h) noexcept {
    return settle_<>(h);
}
template <class... Hs>
TH_INLINE erased_handle settle(choice<Hs...> h) noexcept {
    return settle_<>(h);
}
TH_INLINE erased_handle settle(erased_handle h) noexcept { return h; }
TH_INLINE erased_handle settle(no_operation) noexcept { return {}; }

// The driver: whoever is at the bottom of the machine stack.
template <class H>
TH_INLINE void run(H h) noexcept {
    erased_handle e = settle(h);
    while (e) {
        e = e();
    }
}

// The common type of handles: the same handle; or, for two different ones,
// the erased handle, or in VARIANT mode a choice of all their alternatives.
template <class... Hs>
struct type_list {};

template <class H>
struct alternatives {
    using type = type_list<H>;
};
template <class... Hs>
struct alternatives<choice<Hs...>> {
    using type = type_list<Hs...>;
};

template <class List, class H>
struct add_unique;
template <class... Hs, class H>
struct add_unique<type_list<Hs...>, H> {
    using type =
        std::conditional_t<in_pack<H, Hs...>, type_list<Hs...>, type_list<Hs..., H>>;
};

template <class List, class Alts>
struct union_of;
template <class List>
struct union_of<List, type_list<>> {
    using type = List;
};
template <class List, class H, class... Rest>
struct union_of<List, type_list<H, Rest...>>
    : union_of<typename add_unique<List, H>::type, type_list<Rest...>> {};

template <class List>
struct choice_of;
template <class H>
struct choice_of<type_list<H>> {
    using type = H;
};
template <class... Hs>
struct choice_of<type_list<Hs...>> {
    using type = choice<Hs...>;
};

template <class A, class B>
struct merge {
#if TH_MODE == TH_VARIANT
    using type = std::conditional_t<is_erased<A> || is_erased<B>,
                                    erased_handle,
                                    typename choice_of<typename union_of<
                                        typename alternatives<A>::type,
                                        typename alternatives<B>::type>::type>::type>;
#else
    using type = std::conditional_t<std::same_as<A, B>, A, erased_handle>;
#endif
};

template <class... Hs>
struct common_handle;
template <class H>
struct common_handle<H> {
    using type = H;
};
template <class A, class B, class... Rest>
struct common_handle<A, B, Rest...> : common_handle<typename merge<A, B>::type, Rest...> {
};
template <class... Hs>
using common_handle_t = typename common_handle<Hs...>::type;

// A branch between two handles.
template <class F, class G>
TH_INLINE auto either(bool first, F f, G g) noexcept
    -> common_handle_t<std::invoke_result_t<F>, std::invoke_result_t<G>> {
    if (first) {
        return f();
    }
    return g();
}

// A receiver r of type R provides
//
//   typename R::operation_type      its parent's type
//   r.operation_state()             a pointer to its parent
//   R::complete<Sig>(op)            static: deliver Sig to the parent op, and
//                                   return what runs next
//   r.destination<Sig>()            where Sig's payload goes
//   r.get_env()
//
// Only the child calls the last two, while it and its receiver are alive.

// Delivering a completion at once, as a plain call.
template <class Sig, class R>
TH_INLINE auto complete(R& rcvr) noexcept {
    return R::template complete<Sig>(*rcvr.operation_state());
}

// Delivering a signal from inside another delivery: the one place the mode
// shows. EAGER calls at once (the call nests); the others return a handle.
template <class Sig, class R>
TH_INLINE auto later(R& rcvr) noexcept {
#if TH_MODE == TH_EAGER
    return th::complete<Sig>(rcvr);
#elif TH_MODE == TH_ERASED
    return erased_handle(operation_handle<R, Sig>(rcvr.operation_state()));
#else // TYPED, VARIANT
    return operation_handle<R, Sig>(rcvr.operation_state());
#endif
}

template <class Op>
TH_INLINE auto start_later(Op& op) noexcept {
#if TH_MODE == TH_EAGER
    return op.start();
#elif TH_MODE == TH_ERASED
    return erased_handle(operation_handle<Op, start_t()>(&op));
#else // TYPED, VARIANT
    return operation_handle<Op, start_t()>(&op);
#endif
}

// Starting in tail position is a plain call: start's result is the caller's.
template <class Op>
TH_INLINE auto start(Op& op) noexcept {
    return op.start();
}

template <class S, class R>
auto connect(S&& s, R r) noexcept(noexcept(std::forward<S>(s).connect(std::move(r)))) {
    return std::forward<S>(s).connect(std::move(r));
}
template <class S, class R>
using connect_result_t = decltype(th::connect(std::declval<S>(), std::declval<R>()));

// Storage for a T that someone else constructs (tony's slot<T>).
template <class T>
class slot {
  public:
    slot() noexcept {}
    ~slot() { reset(); }
    slot(slot&&) = delete;
    void* address() noexcept { return &value_; }
    T&    adopt() noexcept {
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
    template <class F>
    T& emplace_from(F f) { // for immovable T, from f's result
        reset();
        ::new (&value_) T(f());
        return adopt();
    }

  private:
    union {
        T value_;
    };
    bool engaged_ = false;
};

} // namespace th

// Typed and erased operation handles.
//
// Every transfer of control in the protocol starts an operation or completes
// a receiver. A handle is such a transfer, deferred:
//
//   operation_handle<Op>              typed: start Op
//   operation_handle<R, Tag(Args...)> typed: complete a receiver of type R
//                                     with Tag(Args...)
//   operation_handle<>                erased: two words, {function, data}
//   no_operation                      typed: there is nothing to run
//   choice<Hs...>                     typed (VARIANT mode only): one of
//                                     several typed handles, with an index
//                                     saying which
//
// A typed handle's type names the code it runs, so a chain of typed handles
// can be followed, and inlined, at compile time. Its data is one pointer to
// the operation state that code runs against, and operation states are
// immovable: the operation being started, or the operation that contains
// the receiver being completed. The receiver itself is never copied: a
// receiver needn't be copyable, and it lives in the child, which may be gone
// by the time the handle runs.
//
// A receiver of type R derives from operation_handle<R, Op>, where Op is the
// operation that contains it. That base holds the Op*, and its
// complete<Sig>() makes the handle for Sig without running anything; the
// handle, when run, calls R's private static resume<Sig>(Op&), which does
// the work. A receiver writes resume, destination<Sig>() and get_env(), but
// never complete, so it can't do its operation's work before the child has
// returned. Completions carry no values: a value is constructed at the
// receiver's destination before the completion is delivered.
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

// The completion tags and lists of signatures are stdexec's.
using stdexec::set_error_t;
using stdexec::set_stopped_t;
using stdexec::set_value_t;
template <class... Sigs>
using completion_signatures = stdexec::completion_signatures<Sigs...>;

template <class... Ts>
class operation_handle;

// The erased handle: returned in two registers on x86-64 and AArch64.
template <>
class operation_handle<> {
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

template <class H>
inline constexpr bool is_erased = std::same_as<H, erased_handle>;
template <class H>
inline constexpr bool is_none = std::same_as<H, no_operation>;

template <class O>
concept operation_state = std::is_object_v<O> && requires(O& o) { o.start(); };

template <operation_state Op>
TH_INLINE erased_handle settle(operation_handle<Op> h) noexcept;
template <class R, class Tag, class... Args>
TH_INLINE erased_handle settle(operation_handle<R, Tag(Args...)> h) noexcept;

// Start Op.
template <operation_state Op>
class operation_handle<Op> {
  public:
    TH_INLINE explicit operation_handle(Op* op) noexcept : op_(op) {}

    TH_INLINE decltype(auto) operator()() const noexcept { return op_->start(); }

    // Erasure: {a function for this Op, the operation as data}.
    operator erased_handle() const noexcept { return {&thunk, static_cast<void*>(op_)}; }

  private:
    static erased_handle thunk(void* data) noexcept {
        return settle(operation_handle(static_cast<Op*>(data)));
    }

    Op* op_;
};

// The base of a receiver of type R, contained in an operation of type Op: a
// handle with no completion chosen yet. It can't be run; complete<Sig>()
// chooses one. Op may be incomplete where R derives from this, as it is for
// a receiver defined inside its operation, so nothing here may constrain it.
template <class R, class Op>
class operation_handle<R, Op> {
  public:
    TH_INLINE explicit operation_handle(Op* op) noexcept : op_(op) {}

    // Makes the handle for Sig; runs nothing.
    template <class Sig>
    TH_INLINE operation_handle<R, Sig> complete() const noexcept {
        return operation_handle<R, Sig>(static_cast<void*>(op_));
    }

  protected:
    TH_INLINE Op* op() const noexcept { return op_; }

  private:
    template <class...>
    friend class operation_handle;

    // The one place the pointer goes back to Op*.
    template <class Sig>
    TH_INLINE static auto run(void* op) noexcept {
        return R::template resume<Sig>(*static_cast<Op*>(op));
    }

    Op* op_;
};

// Complete a receiver of type R with Tag(Args...). The pointer is to R's
// operation; only R's base makes these, and only it turns the pointer back.
template <class R, class Tag, class... Args>
class operation_handle<R, Tag(Args...)> {
  public:
    TH_INLINE decltype(auto) operator()() const noexcept {
        return R::template run<Tag(Args...)>(op_);
    }

    // Erasure: {a function for this R and signature, the operation as data}.
    operator erased_handle() const noexcept { return {&thunk, op_}; }

  private:
    template <class...>
    friend class operation_handle;

    TH_INLINE explicit operation_handle(void* op) noexcept : op_(op) {}

    static erased_handle thunk(void* data) noexcept {
        return settle(operation_handle(data));
    }

    void* op_;
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
template <operation_state Op>
inline constexpr bool is_loop_head<operation_handle<Op>> =
    reaches<operation_handle<Op>, hlist<>, operation_handle<Op>>::value;
template <class R, class Tag, class... Args>
inline constexpr bool is_loop_head<operation_handle<R, Tag(Args...)>> =
    reaches<operation_handle<R, Tag(Args...)>,
            hlist<>,
            operation_handle<R, Tag(Args...)>>::value;

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

template <operation_state Op>
TH_INLINE erased_handle settle(operation_handle<Op> h) noexcept {
    return settle_<>(h);
}
template <class R, class Tag, class... Args>
TH_INLINE erased_handle settle(operation_handle<R, Tag(Args...)> h) noexcept {
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

// A branch among N handles: f.template operator()<I>() for the I equal to i,
// which must be less than N.
template <class H, std::size_t I, std::size_t N, class F>
TH_INLINE H by_index_(int i, F& f) noexcept {
    if constexpr (I + 1 == N) {
        return f.template operator()<I>();
    } else {
        if (i == int(I)) {
            return f.template operator()<I>();
        }
        return by_index_<H, I + 1, N>(i, f);
    }
}
template <std::size_t N, class F>
TH_INLINE auto by_index(int i, F f) noexcept {
    using H = typename decltype([]<std::size_t... I>(std::index_sequence<I...>) {
        return std::type_identity<
            common_handle_t<decltype(std::declval<F&>().template operator()<I>())...>> {};
    }(std::make_index_sequence<N>{}))::type;
    return by_index_<H, 0, N>(i, f);
}

// A receiver r of type R provides
//
//   r.complete<Sig>()      a handle for delivering Sig; runs nothing
//   r.destination<Sig>()   where Sig's payload goes
//   r.get_env()
//
// A receiver deriving from operation_handle<R, Op> gets complete from there.
// The erased boundary's receiver writes its own, returning an erased handle.

// Delivering a completion from inside another delivery: the one place the
// mode shows. EAGER runs the completion at once (the call nests), unless the
// receiver handed back an erased handle; the others return the handle.
template <class Sig, class R>
TH_INLINE auto later(R& rcvr) noexcept {
    auto h = rcvr.template complete<Sig>();
#if TH_MODE == TH_EAGER
    if constexpr (is_erased<decltype(h)>) {
        return h;
    } else {
        return h();
    }
#elif TH_MODE == TH_ERASED
    return erased_handle(h);
#else // TYPED, VARIANT
    return h;
#endif
}

template <class Op>
TH_INLINE auto start_later(Op& op) noexcept {
#if TH_MODE == TH_EAGER
    return op.start();
#elif TH_MODE == TH_ERASED
    return erased_handle(operation_handle<Op>(&op));
#else // TYPED, VARIANT
    return operation_handle<Op>(&op);
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

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
//   choice<Hs...>              typed (VARIANT mode only): one of a few typed
//                              handles, with an index saying which
//
// A typed handle for start_t() holds a pointer to the operation state, which
// is immovable. A typed handle for a completion holds a copy of the receiver,
// which points at its parent, so the child that returned it may be gone by
// the time it runs. Completions carry no values: a value is constructed at
// the receiver's destination before the completion is delivered.
//
// Where two different typed handles meet (a branch that may complete either
// way), the common type is the erased handle, or in VARIANT mode a choice of
// up to TH_MAX_CHOICE typed handles. Following a chain stops at an
// erased handle, at no_operation, or at a type already seen (a cycle). A
// typed handle whose own chain can come back to it heads a loop of known
// types, which runs as a loop. run() loops over erased handles until there
// is nothing left.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>
#include <algorithm>
#include <new>
#include <tuple>

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

struct start_t {};  // the signal an operation state receives
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
  constexpr operation_handle(fn_t fn, void* data) noexcept
      : fn_(fn), data_(data) {}

  explicit operator bool() const noexcept { return fn_ != nullptr; }
  operation_handle operator()() const noexcept { return fn_(data_); }

  friend bool operator==(operation_handle, operation_handle) = default;

 private:
  fn_t fn_ = nullptr;
  void* data_ = nullptr;
};

using erased_handle = operation_handle<>;

struct no_operation {
  constexpr operator erased_handle() const noexcept { return {}; }
};

template <class T, class Sig>
using target_of = std::conditional_t<std::same_as<Sig, start_t()>, T*, T>;

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
  using target_type = target_of<T, Sig>;

  TH_INLINE explicit operation_handle(target_type t) noexcept : t_(t) {}

  TH_INLINE decltype(auto) operator()() const noexcept {
    if constexpr (std::same_as<Sig, start_t()>) {
      return t_->start();
    } else {
      T r = t_;  // a receiver is a value; complete on a copy
      return r.template complete<Sig>();
    }
  }

  // Erasure: {a function for this T and Sig, the target as data}.
  operator erased_handle() const noexcept {
    static_assert(sizeof(target_type) <= sizeof(void*) &&
                      std::is_trivially_copyable_v<target_type>,
                  "erasing needs a target that fits in one pointer");
    void* data = nullptr;
    std::memcpy(&data, &t_, sizeof(target_type));
    return {&thunk, data};
  }

 private:
  static erased_handle thunk(void* data) noexcept {
    target_type t;
    std::memcpy(&t, &data, sizeof(target_type));
    return settle(operation_handle(t));
  }

  target_type t_;
};

template <class... Hs>
class choice;
template <class H>
inline constexpr bool is_choice = false;
template <class... Hs>
inline constexpr bool is_choice<choice<Hs...>> = true;

template <class... Seen, class H>
TH_INLINE erased_handle settle_(H h) noexcept;

#ifndef TH_MAX_CHOICE
#define TH_MAX_CHOICE 4
#endif

// One of a few typed handles (or no_operation). Following it branches on
// the index, and each branch carries on along its own chain at compile time.
template <class... Hs>
class choice {
  static_assert(sizeof...(Hs) >= 2 && sizeof...(Hs) <= TH_MAX_CHOICE);
  static_assert((std::is_trivially_copyable_v<Hs> && ...));

 public:
  template <class H>
    requires(std::same_as<H, Hs> || ...)
  TH_INLINE choice(H h) noexcept : index_(index_of<H>) {
    ::new (storage_) H(h);
  }

  template <class... Gs>  // from a choice of fewer alternatives
  TH_INLINE choice(choice<Gs...> c) noexcept
      : choice(c.visit([](auto h) noexcept { return choice(h); })) {}

  template <class F>
  TH_INLINE decltype(auto) visit(F f) const noexcept {
    return visit_<0>(f);
  }

  operator erased_handle() const noexcept {
    return visit([](auto h) noexcept -> erased_handle { return h; });
  }

 private:
  template <class H, std::size_t I = 0, class First, class... Rest>
  static constexpr unsigned char find() noexcept {
    if constexpr (std::same_as<H, First>) {
      return I;
    } else {
      return find<H, I + 1, Rest...>();
    }
  }
  template <class H>
  static constexpr unsigned char index_of = find<H, 0, Hs...>();

  template <std::size_t I, class F>
  TH_INLINE decltype(auto) visit_(F& f) const noexcept {
    using H = std::tuple_element_t<I, std::tuple<Hs...>>;
    H const& h = *std::launder(reinterpret_cast<H const*>(storage_));
    if constexpr (I + 1 == sizeof...(Hs)) {
      return f(h);
    } else {
      if (index_ == I) {
        return f(h);
      }
      return visit_<I + 1>(f);
    }
  }

  // A tagged union by hand: GCC 13 optimizes std::variant less well here.
  alignas(Hs...) unsigned char storage_[std::max({sizeof(Hs)...})];
  unsigned char index_;
};

// ---- Cycles of known types ------------------------------------------------
// What running a handle can hand back, as types.
template <class... Hs>
struct hlist {};
template <class H>
struct successors {
  using type = hlist<decltype(std::declval<H const&>()())>;
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
    } else if constexpr (std::same_as<S, H> ||
                         (std::same_as<S, Visited> || ...)) {
      return false;
    } else {
      return reaches<Target, hlist<Visited..., H>, S>::value;
    }
  }
  template <class... Ss>
  static constexpr bool any(hlist<Ss...>) noexcept {
    return (via<Ss>() || ...);
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
  TH_INLINE explicit loop_step(erased_handle e) noexcept
      : again(false), exit(e) {}
  bool again;
  union {
    Head next;
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
    return loop_step<Head>(erased_handle(x));  // another cycle: leave
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
    return h;  // a cycle not entered at its head: hand it on, erased
  } else if constexpr (is_loop_head<H>) {
    return loop_<Seen...>(h);
  } else if constexpr (is_choice<H>) {
    return h.visit([](auto alt) noexcept -> erased_handle {
      return settle_<Seen..., H>(alt);
    });
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
// the erased handle, or in VARIANT mode a choice of their alternatives while
// there are at most TH_MAX_CHOICE of them.
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
  using type = std::conditional_t<(std::same_as<H, Hs> || ...),
                                  type_list<Hs...>, type_list<Hs..., H>>;
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
  using type = std::conditional_t<(sizeof...(Hs) <= TH_MAX_CHOICE),
                                  choice<Hs...>, erased_handle>;
};

template <class A, class B>
struct merge {
#if TH_MODE == TH_VARIANT
  using type = std::conditional_t<
      is_erased<A> || is_erased<B>, erased_handle,
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
struct common_handle<A, B, Rest...>
    : common_handle<typename merge<A, B>::type, Rest...> {};
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

// Delivering a signal from inside another delivery: the one place the mode
// shows. EAGER calls at once (the call nests); the others return a handle.
template <class Sig, class R>
TH_INLINE auto later(R& rcvr) noexcept {
#if TH_MODE == TH_EAGER
  return rcvr.template complete<Sig>();
#elif TH_MODE == TH_ERASED
  return erased_handle(operation_handle<R, Sig>(rcvr));
#else  // TYPED, VARIANT
  return operation_handle<R, Sig>(rcvr);
#endif
}

template <class Op>
TH_INLINE auto start_later(Op& op) noexcept {
#if TH_MODE == TH_EAGER
  return op.start();
#elif TH_MODE == TH_ERASED
  return erased_handle(operation_handle<Op, start_t()>(&op));
#else  // TYPED, VARIANT
  return operation_handle<Op, start_t()>(&op);
#endif
}

// Starting in tail position is a plain call: start's result is the caller's.
template <class Op>
TH_INLINE auto start(Op& op) noexcept {
  return op.start();
}

template <class S, class R>
auto connect(S&& s, R r) noexcept(
    noexcept(std::forward<S>(s).connect(std::move(r)))) {
  return std::forward<S>(s).connect(std::move(r));
}
template <class S, class R>
using connect_result_t =
    decltype(th::connect(std::declval<S>(), std::declval<R>()));

// Storage for a T that someone else constructs (tony's slot<T>).
template <class T>
class slot {
 public:
  slot() noexcept {}
  ~slot() { reset(); }
  slot(slot&&) = delete;
  void* address() noexcept { return &value_; }
  T& adopt() noexcept {
    engaged_ = true;
    return value_;
  }
  T& operator*() noexcept { return value_; }
  bool has_value() const noexcept { return engaged_; }
  void reset() noexcept {
    if (engaged_) {
      value_.~T();
      engaged_ = false;
    }
  }
  template <class F>
  T& emplace_from(F f) {  // for immovable T, from f's result
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

}  // namespace th

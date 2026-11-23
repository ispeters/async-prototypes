// What a completion carries, and where it is constructed.
//
// A completion Tag(As...) is delivered in two steps: its payload is
// constructed at the receiver's destination for that signature, then the
// completion is delivered (handle.hpp). The payload is:
//
//   no arguments          nothing (no destination)
//   one argument A        an A, or for a reference A, the address of what it
//                         refers to
//   several arguments     values<...>, one aggregate per signature, its
//                         elements as above (D4471's unnamed aggregate)
//
// The consumer gets the arguments back in their declared value categories:
// an A as A&&, a T& as T&, a T&& as T&&.
//
// Signatures are stdexec::completion_signatures lists; senders compute them
// with get_completion_signatures<Env>().
#pragma once
#include "handle.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace th {

// ---- Signatures -----------------------------------------------------------
template <class Sig>
struct sig_traits;
template <class Tag, class... As>
struct sig_traits<Tag(As...)> {
  using tag = Tag;
  using args = std::tuple<As...>;
  static constexpr std::size_t size = sizeof...(As);
  static constexpr bool has_references = (std::is_reference_v<As> || ...);
  using decayed = Tag(std::decay_t<As>...);
};
template <class Sig>
using tag_of_t = typename sig_traits<Sig>::tag;
template <class Sig, std::size_t I>
using arg_t = std::tuple_element_t<I, typename sig_traits<Sig>::args>;

// Tag() for void, else Tag(U).
template <class Tag, class U>
struct result_sig {
  using type = Tag(U);
};
template <class Tag>
struct result_sig<Tag, void> {
  using type = Tag();
};
template <class Tag, class U>
using result_sig_t = typename result_sig<Tag, U>::type;

template <class S, class Env>
using completions_of_t = decltype(std::remove_cvref_t<
                                  S>::template get_completion_signatures<Env>());

using eptr_sigs = completion_signatures<set_error_t(std::exception_ptr)>;
template <bool Nothrow>
using eptr_unless =
    std::conditional_t<Nothrow, completion_signatures<>, eptr_sigs>;

template <class... Lists>
using concat_t = decltype(exec::concat_completion_signatures(Lists()...));

// Lists of signatures as type_lists, and what's in them.
template <class Sigs>
struct as_list;
template <class... S>
struct as_list<completion_signatures<S...>> {
  using type = type_list<S...>;
};
template <class Sigs>
using as_list_t = typename as_list<Sigs>::type;

template <class... A, class... B>
type_list<A..., B...> operator+(type_list<A...>, type_list<B...>);

template <class Tag, class List>
struct select_tag;
template <class Tag, class... S>
struct select_tag<Tag, type_list<S...>> {
  using type = decltype((type_list<>() + ... +
                         std::conditional_t<std::same_as<tag_of_t<S>, Tag>,
                                            type_list<S>, type_list<>>()));
};
// The signatures in a completion_signatures list that have this tag.
template <class Tag, class Sigs>
using select_t = typename select_tag<Tag, as_list_t<Sigs>>::type;

template <class Sig, class List>
inline constexpr std::size_t index_in = 0;
template <class Sig, class First, class... Rest>
inline constexpr std::size_t index_in<Sig, type_list<First, Rest...>> =
    std::same_as<Sig, First> ? 0 : 1 + index_in<Sig, type_list<Rest...>>;

template <class List>
inline constexpr std::size_t size_of = 0;
template <class... S>
inline constexpr std::size_t size_of<type_list<S...>> = sizeof...(S);

template <std::size_t I, class List>
struct nth;
template <std::size_t I, class... S>
struct nth<I, type_list<S...>> {
  using type = std::tuple_element_t<I, std::tuple<S...>>;
};
template <std::size_t I, class List>
using nth_t = typename nth<I, List>::type;

// Call f.template operator()<I>() for each I < N; the results are ignored.
template <std::size_t N, class F>
constexpr void for_each_index(F&& f) {
  [&]<std::size_t... I>(std::index_sequence<I...>) {
    (f.template operator()<I>(), ...);
  }(std::make_index_sequence<N>{});
}

// ---- Payloads -------------------------------------------------------------
template <class A>
using stored_t = std::conditional_t<std::is_reference_v<A>,
                                    std::remove_reference_t<A>*, A>;

template <class T>
union uninit {
  uninit() noexcept {}
  ~uninit() {}
  T value;
};

// The aggregate of several values: each element is constructed in place, on
// its own; the whole is destroyed together.
template <class... Ts>
class values {
 public:
  values() noexcept {}  // constructs no element
  values(values&&) = delete;
  ~values() { destroy(std::index_sequence_for<Ts...>{}); }

  template <std::size_t I>
  auto& get() noexcept {
    return std::get<I>(elems_).value;
  }
  template <std::size_t I>
  void* address() noexcept {
    return std::addressof(std::get<I>(elems_).value);
  }

 private:
  template <std::size_t... I>
  void destroy(std::index_sequence<I...>) noexcept {
    (std::destroy_at(std::addressof(get<I>())), ...);
  }
  std::tuple<uninit<Ts>...> elems_;
};

template <class Sig>
struct payload;
template <class Tag>
struct payload<Tag()> {
  using type = void;
};
template <class Tag, class A>
struct payload<Tag(A)> {
  using type = stored_t<A>;
};
template <class Tag, class A, class B, class... Cs>
struct payload<Tag(A, B, Cs...)> {
  using type = values<stored_t<A>, stored_t<B>, stored_t<Cs>...>;
};
template <class Sig>
using payload_t = typename payload<Sig>::type;
template <class Sig>
inline constexpr bool has_payload = !std::is_void_v<payload_t<Sig>>;

template <class T>
struct size_align {
  static constexpr std::size_t size = sizeof(T), align = alignof(T);
};
template <>
struct size_align<void> {
  static constexpr std::size_t size = 1, align = 1;
};
template <class Sig>
inline constexpr std::size_t payload_size = size_align<payload_t<Sig>>::size;
template <class Sig>
inline constexpr std::size_t payload_align = size_align<payload_t<Sig>>::align;

// Storage for one payload at a time, of any of these signatures. Which one
// is there is the business of whoever uses it: usually the completion that
// arrives says so.
template <class... Sigs>
class payload_storage {
 public:
  void* address() noexcept { return bytes_; }

 private:
  alignas(std::max({std::size_t(1), payload_align<Sigs>...})) std::byte
      bytes_[std::max({std::size_t(1), payload_size<Sigs>...})];
};
template <class List>
struct storage_for;
template <class... S>
struct storage_for<type_list<S...>> {
  using type = payload_storage<S...>;
};
template <class List>
using storage_for_t = typename storage_for<List>::type;

// Store one argument of declared type A at p.
template <class A, class X>
TH_INLINE void store_at(void* p, X&& x) {
  if constexpr (std::is_reference_v<A>) {
    ::new (p) stored_t<A>(std::addressof(x));
  } else {
    ::new (p) A(std::forward<X>(x));
  }
}

template <class Sig, class X>
inline constexpr bool nothrow_store =
    std::is_reference_v<Sig> || std::is_nothrow_constructible_v<Sig, X>;

template <class Sig, class... Xs>
inline constexpr bool nothrow_payload = [] {
  if constexpr (sig_traits<Sig>::size != sizeof...(Xs)) {
    return false;
  } else {
    return []<std::size_t... I>(std::index_sequence<I...>) {
      return (nothrow_store<arg_t<Sig, I>, Xs> && ...);
    }(std::index_sequence_for<Xs...>{});
  }
}();

// Construct Sig's payload at p from one expression per argument.
template <class Sig, class... Xs>
TH_INLINE void construct_payload(void* p, Xs&&... xs) {
  constexpr std::size_t n = sig_traits<Sig>::size;
  static_assert(n == sizeof...(Xs));
  if constexpr (n == 1) {
    (store_at<arg_t<Sig, 0>>(p, std::forward<Xs>(xs)), ...);
  } else if constexpr (n > 1) {
    auto* v = ::new (p) payload_t<Sig>;
    auto refs = std::forward_as_tuple(std::forward<Xs>(xs)...);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
      (store_at<arg_t<Sig, I>>(v->template address<I>(),
                               std::get<I>(std::move(refs))),
       ...);
    }(std::make_index_sequence<n>{});
  }
}

// Construct the payload of a signature with at most one argument from g()'s
// result, so that an immovable result is constructed in place. With no
// argument, just call g.
template <class Sig, class G>
TH_INLINE void construct_payload_from(void* p, G&& g) {
  if constexpr (sig_traits<Sig>::size == 0) {
    std::forward<G>(g)();
  } else {
    static_assert(sig_traits<Sig>::size == 1);
    using A = arg_t<Sig, 0>;
    if constexpr (std::is_reference_v<A>) {
      A r = std::forward<G>(g)();
      ::new (p) stored_t<A>(std::addressof(r));
    } else {
      ::new (p) A(std::forward<G>(g)());
    }
  }
}

template <class A, class S>
TH_INLINE decltype(auto) unstore(S& s) noexcept {
  if constexpr (std::is_reference_v<A>) {
    return static_cast<A>(*s);
  } else {
    return static_cast<A&&>(s);
  }
}

// Call f with Sig's arguments, from its payload at p, in their declared
// value categories.
template <class Sig, class F>
TH_INLINE decltype(auto) apply_payload(void* p, F&& f) {
  constexpr std::size_t n = sig_traits<Sig>::size;
  if constexpr (n == 0) {
    return std::invoke(std::forward<F>(f));
  } else if constexpr (n == 1) {
    return std::invoke(std::forward<F>(f),
                       unstore<arg_t<Sig, 0>>(*static_cast<payload_t<Sig>*>(p)));
  } else {
    auto& v = *static_cast<payload_t<Sig>*>(p);
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
      return std::invoke(std::forward<F>(f),
                         unstore<arg_t<Sig, I>>(v.template get<I>())...);
    }(std::make_index_sequence<n>{});
  }
}

// Call f with lvalues of a payload's elements (no references in Sig).
template <class Sig, class F>
TH_INLINE decltype(auto) apply_lvalues(void* p, F&& f) {
  static_assert(!sig_traits<Sig>::has_references);
  constexpr std::size_t n = sig_traits<Sig>::size;
  if constexpr (n == 0) {
    return std::invoke(std::forward<F>(f));
  } else if constexpr (n == 1) {
    return std::invoke(std::forward<F>(f), *static_cast<payload_t<Sig>*>(p));
  } else {
    auto& v = *static_cast<payload_t<Sig>*>(p);
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> decltype(auto) {
      return std::invoke(std::forward<F>(f), v.template get<I>()...);
    }(std::make_index_sequence<n>{});
  }
}

template <class Sig>
TH_INLINE void destroy_payload(void* p) noexcept {
  if constexpr (has_payload<Sig>) {
    std::destroy_at(static_cast<payload_t<Sig>*>(p));
  }
}

template <class F, class Sig>
struct apply_result;
template <class F, class Tag, class... As>
struct apply_result<F, Tag(As...)> {
  using type = std::invoke_result_t<F, As...>;
  static constexpr bool nothrow = std::is_nothrow_invocable_v<F, As...>;
};
template <class F, class Sig>
using apply_result_t = typename apply_result<F, Sig>::type;

// The same with lvalues of the arguments (as apply_lvalues calls F).
template <class F, class Sig>
struct lvalue_apply;
template <class F, class Tag, class... As>
struct lvalue_apply<F, Tag(As...)> {
  using type = std::invoke_result_t<F, As&...>;
  static constexpr bool nothrow = std::is_nothrow_invocable_v<F, As&...>;
};
template <class F, class Sig>
inline constexpr bool nothrow_apply = apply_result<F, Sig>::nothrow;

// ---- Originating completions ----------------------------------------------
template <class R, class Sig>
TH_INLINE void* destination_of(R& r) noexcept {
  return r.template destination<Sig>();
}

// Construct the payload, if there is one, then deliver. Forwarding a
// completion whose payload a child already constructed is later<Sig>(rcvr)
// alone.
template <class Sig, class R, class... Xs>
TH_INLINE auto complete_with(R& rcvr, Xs&&... xs) noexcept {
  static_assert(nothrow_payload<Sig, Xs...>);
  if constexpr (has_payload<Sig>) {
    construct_payload<Sig>(destination_of<R, Sig>(rcvr), std::forward<Xs>(xs)...);
  }
  return later<Sig>(rcvr);
}

// As complete_with, but if constructing the payload may throw, an exception
// becomes set_error_t(exception_ptr).
template <class Sig, class R, class... Xs>
TH_INLINE auto try_complete_with(R& rcvr, Xs&&... xs) noexcept {
  if constexpr (nothrow_payload<Sig, Xs...>) {
    return complete_with<Sig>(rcvr, std::forward<Xs>(xs)...);
  } else {
    bool ok = true;
    try {
      construct_payload<Sig>(destination_of<R, Sig>(rcvr), std::forward<Xs>(xs)...);
    } catch (...) {
      ::new (destination_of<R, set_error_t(std::exception_ptr)>(rcvr))
          std::exception_ptr(std::current_exception());
      ok = false;
    }
    return either(
        ok, [&] { return later<Sig>(rcvr); },
        [&] { return later<set_error_t(std::exception_ptr)>(rcvr); });
  }
}

template <class R, class... Xs>
TH_INLINE auto set_value(R& rcvr, Xs&&... xs) noexcept {
  return try_complete_with<set_value_t(std::decay_t<Xs>...)>(
      rcvr, std::forward<Xs>(xs)...);
}
template <class R, class E>
TH_INLINE auto set_error(R& rcvr, E&& e) noexcept {
  return try_complete_with<set_error_t(std::decay_t<E>)>(rcvr,
                                                          std::forward<E>(e));
}
template <class R>
TH_INLINE auto set_stopped(R& rcvr) noexcept {
  return complete_with<set_stopped_t()>(rcvr);
}

}  // namespace th

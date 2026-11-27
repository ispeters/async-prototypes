// Sender algorithms against handle.hpp and payload.hpp, written once for
// every mode, for any completion signatures the standard allows:
//
//   just, just_error, just_stopped, just_unless_stopped
//   then, upon_error, upon_stopped
//   let_value, let_error, let_stopped
//   when_all (any number of children)
//   repeat_until (stdexec's exec::repeat_until, not the standard's)
//   erase_boundary, a root
//
// A start or completion returns whatever handle it ends with: in TYPED mode
// a typed handle where only one outcome is possible, and the erased handle
// where outcomes branch (either(), common_handle_t). A receiver's
// environment comes from its parent through get_env(). A receiver derives
// from operation_handle<R, Op>, writing resume<Sig>() for the work a
// completion does; receivers are moved, never copied, and what a completion
// needs lives in an operation state.
#pragma once
#include "env.hpp"
#include "handle.hpp"
#include "payload.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <new>
#include <tuple>
#include <type_traits>
#include <variant>

namespace th {
using std::exception_ptr;
using std::same_as;

template <class Sig, class R>
using later_t = decltype(later<Sig>(std::declval<R&>()));

// A receiver with environment Env, to ask whether connecting to one may
// throw while computing completion signatures (connect is checked again
// against the real receiver).
template <class Env>
struct probe_receiver {
    template <class Sig>
    void* destination() noexcept;
    template <class Sig>
    no_operation complete() const noexcept;
    Env          get_env() const noexcept;
};
template <class S, class Env>
inline constexpr bool nothrow_connect =
    noexcept(th::connect(std::declval<S>(), std::declval<probe_receiver<Env>>()));

// Apply a transform to the signatures of one channel, keeping the rest.
template <class Tag, class Sigs, class Fn>
consteval auto transform_channel(Sigs sigs, Fn fn) {
    if constexpr (same_as<Tag, set_value_t>) {
        return exec::transform_completion_signatures(sigs, fn);
    } else if constexpr (same_as<Tag, set_error_t>) {
        return exec::transform_completion_signatures(sigs, {}, fn);
    } else {
        return exec::transform_completion_signatures(sigs, {}, {}, fn);
    }
}

// ---- just, just_error, just_stopped ---------------------------------------
template <class Tag, class... Ts>
struct just_sender {
    using sig                     = Tag(Ts...);
    static constexpr bool nothrow = (std::is_nothrow_move_constructible_v<Ts> && ...);
    std::tuple<Ts...>     vals;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return concat_t<completion_signatures<sig>, eptr_unless<nothrow>>();
    }

    template <class R>
    struct op {
        R                 rcvr;
        std::tuple<Ts...> vals;

        TH_INLINE auto start() & noexcept {
            return std::apply(
                [&](Ts&... vs) { return try_complete_with<sig>(rcvr, std::move(vs)...); },
                vals);
        }
    };

    template <class R>
    op<R> connect(R rcvr) && noexcept(nothrow) {
        return {std::move(rcvr), std::move(vals)};
    }
};
template <class... Ts>
just_sender<set_value_t, Ts...> just(Ts... ts) {
    return {{std::move(ts)...}};
}
template <class E>
just_sender<set_error_t, E> just_error(E e) {
    return {{std::move(e)}};
}
inline just_sender<set_stopped_t> just_stopped() { return {}; }

// ---- just_unless_stopped ------------------------------------------------
// A leaf that asks its receiver's stop token: stopped if stop has been
// requested, else values. Against a token that can't be stopped it is just,
// and doesn't declare set_stopped_t().
template <class... Ts>
struct just_unless_stopped_sender {
    using sig                     = set_value_t(Ts...);
    static constexpr bool nothrow = (std::is_nothrow_move_constructible_v<Ts> && ...);
    std::tuple<Ts...>     vals;

    template <class Env>
    static consteval auto get_completion_signatures() {
        using stopped = std::conditional_t<
            unstoppable<decltype(get_stop_token(std::declval<const Env&>()))>,
            completion_signatures<>,
            completion_signatures<set_stopped_t()>>;
        return concat_t<completion_signatures<sig>, eptr_unless<nothrow>, stopped>();
    }

    template <class R>
    struct op {
        R                 rcvr;
        std::tuple<Ts...> vals;

        TH_INLINE auto values() noexcept {
            return std::apply(
                [&](Ts&... vs) { return try_complete_with<sig>(rcvr, std::move(vs)...); },
                vals);
        }

        TH_INLINE auto start() & noexcept {
            if constexpr (unstoppable<stop_token_of_t<R>>) {
                return values();
            } else {
                return either(
                    get_stop_token(th::get_env(rcvr)).stop_requested(),
                    [&] { return th::set_stopped(rcvr); },
                    [&] { return values(); });
            }
        }
    };

    template <class R>
    op<R> connect(R rcvr) && noexcept(nothrow) {
        return {std::move(rcvr), std::move(vals)};
    }
};
template <class... Ts>
just_unless_stopped_sender<Ts...> just_unless_stopped(Ts... ts) {
    return {{std::move(ts)...}};
}

// ---- then, upon_error, upon_stopped ---------------------------------------
// F applied to every completion of one channel (Tag); the rest pass through.
template <class F, class... As>
consteval auto then_result_sigs() {
    using U = std::invoke_result_t<F, As...>;
    return concat_t<completion_signatures<result_sig_t<set_value_t, U>>,
                    eptr_unless<std::is_nothrow_invocable_v<F, As...>>>();
}

template <class Tag, class Child, class F, class R>
struct then_op {
    using child_sigs = completions_of_t<Child, env_of_t<R>>;
    using filtered   = select_t<Tag, child_sigs>; // the completions F handles

    struct rcvr_t : operation_handle<rcvr_t, then_op> {
        using base_t = operation_handle<rcvr_t, then_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<tag_of_t<Sig>, Tag>) {
                return this->op()->held.address(); // filtered: F needs it
            } else {
                return destination_of<R, Sig>(this->op()->rcvr); // passed through
            }
        }

        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(then_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, Tag>) {
                return self.template invoke<Sig>();
            } else {
                return later<Sig>(self.rcvr);
            }
        }
    };

    F                               f;
    R                               rcvr;
    storage_for_t<filtered>         held; // the child's payload, whichever it is
    connect_result_t<Child, rcvr_t> child;

    then_op(Child&& c, F fn, R r) noexcept(std::is_nothrow_move_constructible_v<F> &&
                                           noexcept(th::connect(std::declval<Child>(),
                                                                std::declval<rcvr_t>())))
        : f(std::move(fn)),
          rcvr(std::move(r)),
          child(th::connect(std::move(c), rcvr_t{this})) {}
    then_op(then_op&&) = delete;

    TH_INLINE auto start() & noexcept { return th::start(child); }

    template <class Sig>
    TH_INLINE auto invoke() noexcept {
        using U    = apply_result_t<F, Sig>;
        using Out  = result_sig_t<set_value_t, U>;
        void* p    = held.address();
        auto  call = [&]() -> decltype(auto) {
            return apply_payload<Sig>(p, std::move(f));
        };
        auto dest = [&]() noexcept -> void* {
            if constexpr (has_payload<Out>) {
                return destination_of<R, Out>(rcvr);
            } else {
                return nullptr;
            }
        };
        if constexpr (nothrow_apply<F, Sig>) {
            construct_payload_from<Out>(dest(), call);
            destroy_payload<Sig>(p); // before completing: the parent may destroy *this
            return later<Out>(rcvr);
        } else {
            bool ok = true;
            try {
                construct_payload_from<Out>(dest(), call);
            } catch (...) {
                ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
                    exception_ptr(std::current_exception());
                ok = false;
            }
            destroy_payload<Sig>(p);
            return either(
                ok,
                [&] { return later<Out>(rcvr); },
                [&] { return later<set_error_t(exception_ptr)>(rcvr); });
        }
    }
};

template <class Tag, class Child, class F>
struct then_sender {
    Child child;
    F     f;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return transform_channel<Tag>(completions_of_t<Child, Env>(), []<class... As>() {
            return then_result_sigs<F, As...>();
        });
    }

    template <class R>
    then_op<Tag, Child, F, R> connect(R rcvr) && noexcept(
        std::is_nothrow_constructible_v<then_op<Tag, Child, F, R>, Child, F, R>) {
        return {std::move(child), std::move(f), std::move(rcvr)};
    }
};
template <class Child, class F>
then_sender<set_value_t, Child, F> then(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class Child, class F>
then_sender<set_error_t, Child, F> upon_error(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class Child, class F>
then_sender<set_stopped_t, Child, F> upon_stopped(Child c, F f) {
    return {std::move(c), std::move(f)};
}

// ---- let_value, let_error, let_stopped ------------------------------------
// For each completion of one channel (Tag), F is called with lvalues of the
// completion's decayed arguments, which live until this operation state is
// destroyed, and the sender it returns is connected and started.
template <class F, class Env, class Sig>
struct let_step {
    using decayed                      = typename sig_traits<Sig>::decayed;
    using second                       = typename lvalue_apply<F&, decayed>::type;
    static constexpr bool nothrow_call = lvalue_apply<F&, decayed>::nothrow;
    // Copying the arguments (if they are references), calling F, connecting.
    static constexpr bool nothrow_copy = [] {
        if constexpr (!sig_traits<Sig>::has_references) {
            return true;
        } else {
            return []<std::size_t... I>(std::index_sequence<I...>) {
                return (std::is_nothrow_constructible_v<std::decay_t<arg_t<Sig, I>>,
                                                        arg_t<Sig, I>> &&
                        ...);
            }(std::make_index_sequence<sig_traits<Sig>::size>{});
        }
    }();
};

template <class F, class Env, class... As>
consteval auto let_result_sigs() {
    using step   = let_step<F, Env, set_value_t(As...)>; // the tag doesn't matter
    using second = typename step::second;
    constexpr bool nothrow =
        step::nothrow_copy && step::nothrow_call && nothrow_connect<second, Env>;
    return concat_t<completions_of_t<second, Env>, eptr_unless<nothrow>>();
}

template <class Tag, class Child, class F, class R>
struct let_op {
    using child_sigs               = completions_of_t<Child, env_of_t<R>>;
    using filtered                 = select_t<Tag, child_sigs>;
    static constexpr std::size_t n = size_of<filtered>;
    template <std::size_t I>
    using sig_at = nth_t<I, filtered>;
    template <std::size_t I>
    using step_at = let_step<F, env_of_t<R>, sig_at<I>>;

    struct rcvr1_t : operation_handle<rcvr1_t, let_op> { // the first child's
        using base_t = operation_handle<rcvr1_t, let_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (!same_as<tag_of_t<Sig>, Tag>) {
                return destination_of<R, Sig>(this->op()->rcvr);
            } else if constexpr (sig_traits<Sig>::has_references) {
                return this->op()->scratch.address(); // addresses, copied in next()
            } else {
                return this->op()->kept.address(); // kept: F's sender may borrow it
            }
        }

        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(let_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, Tag>) {
                return self.template next<index_in<Sig, filtered>>();
            } else {
                return later<Sig>(self.rcvr);
            }
        }
    };

    struct rcvr2_t : operation_handle<rcvr2_t, let_op> { // the second child's: everything
                                                         // passes through
        using base_t = operation_handle<rcvr2_t, let_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            return destination_of<R, Sig>(this->op()->rcvr);
        }
        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(let_op& self) noexcept {
            return later<Sig>(self.rcvr);
        }
    };

    template <std::size_t I>
    using op2_t = connect_result_t<typename step_at<I>::second, rcvr2_t>;
    template <std::size_t I>
    static constexpr bool nothrow_next =
        step_at<I>::nothrow_copy && step_at<I>::nothrow_call &&
        noexcept(th::connect(std::declval<typename step_at<I>::second>(),
                             std::declval<rcvr2_t>()));

    template <class>
    struct sizes;
    template <std::size_t... I>
    struct sizes<std::index_sequence<I...>> {
        static constexpr std::size_t op_size =
            std::max({std::size_t(1), sizeof(op2_t<I>)...});
        static constexpr std::size_t op_align =
            std::max({std::size_t(1), alignof(op2_t<I>)...});
        using kept_t    = payload_storage<typename step_at<I>::decayed...>;
        using scratch_t = payload_storage<sig_at<I>...>;
    };
    using sz = sizes<std::make_index_sequence<n>>;

    F                                      f;
    R                                      rcvr;
    slot<connect_result_t<Child, rcvr1_t>> first;
    typename sz::kept_t    kept; // the decayed arguments, while the second runs
    typename sz::scratch_t scratch;
    alignas(sz::op_align) std::byte second[sz::op_size];
    int live = -1; // which completion's arguments and second are in place

    let_op(Child&& c, F fn, R r) : f(std::move(fn)), rcvr(std::move(r)) {
        first.emplace_from([&] { return th::connect(std::move(c), rcvr1_t{this}); });
    }
    let_op(let_op&&) = delete;
    ~let_op() {
        // The destructor runs outside any completion, so here, and only here,
        // which arguments are in place has to be remembered.
        for_each_index<n>([&]<std::size_t I>() {
            if (live == int(I)) {
                std::destroy_at(std::launder(reinterpret_cast<op2_t<I>*>(second)));
                destroy_payload<typename step_at<I>::decayed>(kept.address());
            }
        });
    }

    TH_INLINE auto start() & noexcept { return th::start(*first); }

    template <std::size_t I>
    TH_INLINE auto next() noexcept {
        using Sig = sig_at<I>;
        using D   = typename step_at<I>::decayed;
        using Op2 = op2_t<I>;
        auto make = [&] {
            if constexpr (sig_traits<Sig>::has_references) { // copy before the
                apply_payload<Sig>(scratch.address(), [&](auto&&... as) { // child goes
                    construct_payload<D>(kept.address(), as...);
                });
            }
            first.reset(); // the first child has completed
            live = int(I);
            return ::new (static_cast<void*>(second))
                Op2(th::connect(apply_lvalues<D>(kept.address(), f), rcvr2_t{this}));
        };
        if constexpr (nothrow_next<I>) {
            return start_later(*make());
        } else {
            Op2* s = nullptr;
            try {
                s = make();
            } catch (...) {
                ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
                    exception_ptr(std::current_exception());
            }
            if (!s && live == int(I)) { // F or connect threw: drop the arguments
                destroy_payload<D>(kept.address());
                live = -1;
            }
            return either(
                s != nullptr,
                [&] { return start_later(*s); },
                [&] { return later<set_error_t(exception_ptr)>(rcvr); });
        }
    }
};

template <class Tag, class Child, class F>
struct let_sender {
    Child child;
    F     f;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return transform_channel<Tag>(completions_of_t<Child, Env>(), []<class... As>() {
            return let_result_sigs<F, Env, As...>();
        });
    }

    template <class R>
    let_op<Tag, Child, F, R> connect(R rcvr) && {
        return {std::move(child), std::move(f), std::move(rcvr)};
    }
};
template <class Child, class F>
let_sender<set_value_t, Child, F> let_value(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class Child, class F>
let_sender<set_error_t, Child, F> let_error(Child c, F f) {
    return {std::move(c), std::move(f)};
}
template <class Child, class F>
let_sender<set_stopped_t, Child, F> let_stopped(Child c, F f) {
    return {std::move(c), std::move(f)};
}

// ---- when_all -------------------------------------------------------------
// Each child may have at most one value completion. If every child has one,
// the result is one value completion with every child's decayed values, in
// order; if any has none, when_all has no value completion. A child whose value
// completion is a single non-reference value constructs it straight into
// the parent's storage; any other is staged and moved in (or copied, for
// references) when all have succeeded.
template <class... Lists>
struct flatten_values;
template <class... As>
struct flatten_values<set_value_t(As...)> {
    using type = set_value_t(As...);
};
template <class... As, class... Bs, class... Rest>
struct flatten_values<set_value_t(As...), set_value_t(Bs...), Rest...>
    : flatten_values<set_value_t(As..., Bs...), Rest...> {};

using when_all_env_prop = prop<get_stop_token_t, inplace_stop_token>;
template <class Env>
using when_all_env_t = env<when_all_env_prop, Env>;

// constructed straight into the result
template <class Sig>
inline constexpr bool direct_value =
    sig_traits<Sig>::size == 1 && !sig_traits<Sig>::has_references;

template <class Env, class... Ss>
struct when_all_types {
    using env_t = when_all_env_t<Env>;
    template <class S>
    using sigs = completions_of_t<S, env_t>;
    template <class S>
    using value_list = select_t<set_value_t, sigs<S>>;
    static_assert(((size_of<value_list<Ss>> <= 1) && ...),
                  "when_all's children may have at most one value completion");
    static constexpr bool has_values = ((size_of<value_list<Ss>> == 1) && ...);
    // set_value_t() for a child with none: never used
    template <class S>
    using value_sig =
        typename std::conditional_t<size_of<value_list<S>> == 1,
                                    nth<0, value_list<S>>,
                                    std::type_identity<set_value_t()>>::type;
    using result_sig =
        typename flatten_values<typename sig_traits<value_sig<Ss>>::decayed...>::type;
    // Staged values are moved in, references copied: either may throw.
    static constexpr bool nothrow_values = [] {
        return ([]<class S>(S*) {
            using V = value_sig<S>;
            if constexpr (direct_value<V>) {
                return true;
            } else {
                return []<std::size_t... I>(std::index_sequence<I...>) {
                    return (std::is_nothrow_constructible_v<std::decay_t<arg_t<V, I>>,
                                                            arg_t<V, I>> &&
                            ...);
                }(std::make_index_sequence<sig_traits<V>::size>{});
            }
        }(static_cast<Ss*>(nullptr)) &&
                ...);
    }();
    template <class E>
    using decayed_error_sig = completion_signatures<set_error_t(std::decay_t<E>)>;
    template <class S>
    static consteval auto errors_of() {
        return exec::transform_completion_signatures(
            sigs<S>(),
            exec::ignore_completion(),
            []<class E>() { return decayed_error_sig<E>(); },
            exec::ignore_completion());
    }
    using completions = concat_t<std::conditional_t<has_values,
                                                    completion_signatures<result_sig>,
                                                    completion_signatures<>>,
                                 decltype(errors_of<Ss>())...,
                                 completion_signatures<set_stopped_t()>,
                                 eptr_unless<nothrow_values>>;
};

template <class R, class... Ss>
struct when_all_op {
    using types                    = when_all_types<env_of_t<R>, Ss...>;
    static constexpr std::size_t n = sizeof...(Ss);
    template <std::size_t I>
    using child_t = std::tuple_element_t<I, std::tuple<Ss...>>;
    template <std::size_t I>
    using value_sig = typename types::template value_sig<child_t<I>>;
    template <std::size_t I>
    using error_list = select_t<set_error_t, typename types::template sigs<child_t<I>>>;
    using result_sig = typename types::result_sig;
    enum : int { none, error, stopped };
    // Straight into our parent's storage, if there will be a result.
    template <class Sig>
    static constexpr bool direct = types::has_values && direct_value<Sig>;

    // Where child I's first value goes in the result.
    template <std::size_t I>
    static constexpr std::size_t offset = [] {
        return []<std::size_t... J>(std::index_sequence<J...>) {
            return (std::size_t(0) + ... + sig_traits<value_sig<J>>::size);
        }(std::make_index_sequence<I>{});
    }();

    template <std::size_t I>
    struct rcvr_t : operation_handle<rcvr_t<I>, when_all_op> {
        using base_t = operation_handle<rcvr_t, when_all_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_error_t>) {
                // staged: several may fail
                return this->op()->template part<I>().errors.address();
            } else if constexpr (direct<Sig>) {
                return this->op()
                    ->template element<offset<I>>(); // into our parent's storage
            } else {
                return this->op()->template part<I>().values.address();
            }
        }

        // The children's stop token is when_all's own; the rest is the parent's.
        // (Named, not deduced: a child asks for it while when_all_op is still
        // being defined.)
        TH_INLINE when_all_env_t<env_of_t<R>> get_env() const noexcept {
            return {when_all_env_prop{this->op()->stop_source.get_token()},
                    th::get_env(this->op()->rcvr)};
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(when_all_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_error_t>) {
                self.template part<I>().error_index = int(index_in<Sig, error_list<I>>);
                self.fail(int(I));
            } else if constexpr (same_as<Sig, set_stopped_t()>) {
                self.stop();
            } else {
                self.template part<I>().has_value = true;
            }
            return self.arrive();
        }
    };

    // Per child: its operation state and what it stages.
    template <std::size_t I>
    struct part_t {
        connect_result_t<child_t<I>, rcvr_t<I>> op;
        storage_for_t<error_list<I>>            errors;
        payload_storage<value_sig<I>>           values; // unused for a direct value
        int                                     error_index = -1;
        bool                                    has_value   = false;

        part_t(child_t<I>&& s, when_all_op* self)
            : op(th::connect(std::move(s), rcvr_t<I>{self})) {}
    };
    template <class>
    struct parts_t;
    template <std::size_t... I>
    struct parts_t<std::index_sequence<I...>> : part_t<I>... {
        parts_t(std::tuple<Ss...>&& ss, when_all_op* self)
            : part_t<I>(std::get<I>(std::move(ss)), self)... {}
    };

    // Stop requested of the parent is forwarded to the children, unless the
    // parent's token can't be stopped.
    using parent_token = stop_token_of_t<R>;
    static_assert(unstoppable<parent_token> || same_as<parent_token, inplace_stop_token>,
                  "only inplace_stop_token is forwarded");
    struct forward_stop {
        when_all_op* op;
        void         operator()() noexcept { op->stop_source.request_stop(); }
    };
    using on_stop_t = inplace_stop_callback<forward_stop>;
    struct no_callback {
        void reset() noexcept {}
    };

    R                   rcvr;
    inplace_stop_source stop_source;
    [[no_unique_address]] std::
        conditional_t<unstoppable<parent_token>, no_callback, slot<on_stop_t>> on_stop;
    void*            result = nullptr; // our parent's storage for result_sig
    int              failed = -1;
    std::atomic<int> state{none};
    std::atomic<int> count{int(n)};
    parts_t<std::make_index_sequence<n>> parts;

    when_all_op(std::tuple<Ss...>&& ss, R r)
        : rcvr(std::move(r)), parts(std::move(ss), this) {}
    when_all_op(when_all_op&&) = delete;

    template <std::size_t I>
    part_t<I>& part() noexcept {
        return parts;
    }
    template <std::size_t K>
    void* element() noexcept {
        if constexpr (sig_traits<result_sig>::size == 1) {
            return result;
        } else {
            return static_cast<payload_t<result_sig>*>(result)->template address<K>();
        }
    }

    TH_INLINE auto start() & noexcept {
        if constexpr (!unstoppable<parent_token>) {
            on_stop.emplace_from([&] {
                return on_stop_t(get_stop_token(th::get_env(rcvr)), forward_stop{this});
            });
        }
        if constexpr (types::has_values && has_payload<result_sig>) {
            result = destination_of<R, result_sig>(rcvr);
            if constexpr (sig_traits<result_sig>::size > 1) {
                ::new (result) payload_t<result_sig>; // constructs no element
            }
        }
        // A fork: run what each child but the last hands back.
        for_each_index<n - 1>([&]<std::size_t I>() { th::run(th::start(part<I>().op)); });
        return th::start(part<n - 1>().op);
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

    // Move or copy child I's staged values into the result.
    template <std::size_t I>
    void move_in() {
        using V = value_sig<I>;
        if constexpr (!direct<V>) {
            void* staged = part<I>().values.address();
            [&]<std::size_t... J>(std::index_sequence<J...>) {
                apply_payload<V>(staged, [&](auto&&... as) {
                    (::new (element<offset<I> + J>())
                         std::decay_t<arg_t<V, J>>(static_cast<decltype(as)&&>(as)),
                     ...);
                });
            }(std::make_index_sequence<sig_traits<V>::size>{});
            destroy_payload<V>(staged);
            part<I>().has_value = false;
        }
    }

    // Abandon whatever values arrived.
    void abandon() noexcept {
        for_each_index<n>([&]<std::size_t I>() {
            using V = value_sig<I>;
            if (!part<I>().has_value) {
                return;
            }
            if constexpr (direct<V>) {
                std::destroy_at(
                    static_cast<std::decay_t<arg_t<V, 0>>*>(element<offset<I>>()));
            } else {
                destroy_payload<V>(part<I>().values.address());
            }
        });
    }

    template <class E>
    using error_handle_t = later_t<set_error_t(std::decay_t<E>), R>;
    template <std::size_t I, class List>
    struct error_handles;
    template <std::size_t I, class... Es>
    struct error_handles<I, type_list<set_error_t(Es)...>> {
        using type = type_list<error_handle_t<Es>...>;
    };
    template <class... Lists>
    struct arrive_type;
    template <class... Hs>
    struct arrive_type<type_list<Hs...>> {
        using value_t =
            typename std::conditional_t<types::has_values,
                                        std::type_identity<later_t<result_sig, R>>,
                                        std::type_identity<no_operation>>::type;
        using eptr_t = typename std::conditional_t<
            types::nothrow_values,
            std::type_identity<no_operation>,
            std::type_identity<later_t<set_error_t(exception_ptr), R>>>::type;
        using type = common_handle_t<no_operation,
                                     value_t,
                                     later_t<set_stopped_t(), R>,
                                     eptr_t,
                                     Hs...>;
    };
    using arrive_t = typename arrive_type<decltype((
        type_list<>() + ... +
        typename error_handles<
            0,
            select_t<set_error_t, typename types::template sigs<Ss>>>::type()))>::type;

    // Hand on child I's error J, decayed.
    template <std::size_t I, std::size_t J = 0>
    TH_INLINE arrive_t pass_error() noexcept {
        using List = error_list<I>;
        if constexpr (J == size_of<List>) {
            __builtin_unreachable();
        } else {
            if (part<I>().error_index != int(J)) {
                return pass_error<I, J + 1>();
            }
            using Sig    = nth_t<J, List>;
            using E      = std::decay_t<arg_t<Sig, 0>>;
            void* staged = part<I>().errors.address();
            apply_payload<Sig>(staged, [&](auto&& e) {
                ::new (destination_of<R, set_error_t(E)>(rcvr))
                    E(static_cast<decltype(e)&&>(e));
            });
            destroy_payload<Sig>(staged);
            return later<set_error_t(E)>(rcvr);
        }
    }
    // Destroy the errors staged by children other than the one handed on.
    void drop_errors(int kept) noexcept {
        for_each_index<n>([&]<std::size_t I>() {
            using List = error_list<I>;
            if (int(I) == kept || part<I>().error_index < 0) {
                return;
            }
            for_each_index<size_of<List>>([&]<std::size_t J>() {
                if (part<I>().error_index == int(J)) {
                    destroy_payload<nth_t<J, List>>(part<I>().errors.address());
                }
            });
        });
    }

    template <std::size_t I = 0>
    TH_INLINE arrive_t error_from(int i) noexcept {
        if constexpr (I + 1 == n) {
            return pass_error<I>();
        } else {
            return i == int(I) ? pass_error<I>() : error_from<I + 1>(i);
        }
    }

    TH_INLINE arrive_t arrive() noexcept {
        if (count.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return no_operation{};
        }
        on_stop.reset(); // before completing: the parent may destroy *this
        if constexpr (!types::has_values) {
            // state can't be none: some child can't complete with a value
        } else if (state.load() == none) {
            if constexpr (types::nothrow_values) {
                for_each_index<n>([&]<std::size_t I>() { move_in<I>(); });
                return later<result_sig>(rcvr);
            } else {
                try {
                    for_each_index<n>([&]<std::size_t I>() { move_in<I>(); });
                    return later<result_sig>(rcvr);
                } catch (...) {
                    abandon();
                    ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
                        exception_ptr(std::current_exception());
                    return later<set_error_t(exception_ptr)>(rcvr);
                }
            }
        }
        abandon();
        if (state.load() == stopped) {
            return later<set_stopped_t()>(rcvr);
        }
        drop_errors(failed);
        return error_from(failed);
    }
};

template <class... Ss>
struct when_all_sender {
    std::tuple<Ss...> children;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return typename when_all_types<Env, Ss...>::completions();
    }

    template <class R>
    when_all_op<R, Ss...> connect(R rcvr) && {
        return {std::move(children), std::move(rcvr)};
    }
};
template <class... Ss>
when_all_sender<Ss...> when_all(Ss... ss) {
    return {{std::move(ss)...}};
}

// ---- repeat_until ---------------------------------------------------------
// Restarts its child until the child's value, converted to bool, is true,
// then completes with set_value_t(). The restart is a deferred start
// delivered to the child's new operation state: no scheduler, no added
// completion, no stack growth unless the mode is EAGER.
template <class Child, class R>
struct repeat_until_op {
    struct rcvr_t;
    using child_sigs = completions_of_t<Child, env_of_t<R>>;
    using value_list = select_t<set_value_t, child_sigs>;

    struct rcvr_t : operation_handle<rcvr_t, repeat_until_op> {
        using base_t = operation_handle<rcvr_t, repeat_until_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_value_t>) {
                return this->op()->flag.address();
            } else {
                return destination_of<R, Sig>(this->op()->rcvr);
            }
        }

        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(repeat_until_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_value_t>) {
                bool done =
                    apply_payload<Sig>(self.flag.address(), [](auto&& b) noexcept {
                        return static_cast<bool>(b);
                    });
                destroy_payload<Sig>(self.flag.address());
                return self.again(done);
            } else {
                self.child_op.reset();
                return later<Sig>(self.rcvr);
            }
        }
    };

    using child_op_t = connect_result_t<Child&&, rcvr_t>;
    static constexpr bool nothrow_connect =
        std::is_nothrow_copy_constructible_v<Child> &&
        noexcept(th::connect(std::declval<Child>(), std::declval<rcvr_t>()));

    Child                     child;
    R                         rcvr;
    storage_for_t<value_list> flag;
    slot<child_op_t>          child_op;

    repeat_until_op(Child&& c, R r) : child(std::move(c)), rcvr(std::move(r)) {}
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
                c != nullptr,
                [&] { return how(*c); },
                [&] { return later<set_error_t(exception_ptr)>(rcvr); });
        }
    }

    TH_INLINE auto start() & noexcept {
        return connect_then([](child_op_t& c) { return th::start(c); });
    }

    // again()'s type has to be named: in EAGER mode the restart nests a call
    // that ends in again() itself, so it can't be deduced. A restart returns
    // what ending does there.
    using done_t = later_t<set_value_t(), R>;
#if TH_MODE == TH_EAGER
    using restart_t = done_t;
#else
    using restart_t = decltype(start_later(std::declval<child_op_t&>()));
#endif
    using again_t = std::conditional_t<
        nothrow_connect,
        common_handle_t<done_t, restart_t>,
        common_handle_t<done_t, restart_t, later_t<set_error_t(exception_ptr), R>>>;

    // One brace after both headers, so that tools that don't evaluate the
    // condition still see one function body.
#ifdef TH_DEDUCE_AGAIN // see CMakeLists.txt
    TH_INLINE auto again(bool done) noexcept
#else
    TH_INLINE again_t again(bool done) noexcept
#endif
    {
        child_op.reset(); // the child has completed
        return either(
            done,
            [&] { return later<set_value_t()>(rcvr); },
            [&] { return connect_then([](child_op_t& c) { return start_later(c); }); });
    }
};

template <class Child>
struct repeat_until_sender {
    Child child;

    template <class Env>
    static consteval auto get_completion_signatures() {
        constexpr bool nothrow =
            std::is_nothrow_copy_constructible_v<Child> && nothrow_connect<Child, Env>;
        return concat_t<completion_signatures<set_value_t()>,
                        decltype(exec::transform_completion_signatures(
                            completions_of_t<Child, Env>(), exec::ignore_completion())),
                        eptr_unless<nothrow>>();
    }

    template <class R>
    repeat_until_op<Child, R> connect(R rcvr) && {
        return {std::move(child), std::move(rcvr)};
    }
};
template <class Child>
repeat_until_sender<Child> repeat_until(Child c) {
    return {std::move(c)};
}

// ---- an erased boundary ----------------------------------------------------
// The caller's continuation record: one pointer to a static table with one
// resume function and one destination function per completion signature
// (in the order the child declares them: the index is local to this
// boundary), and the one query forwarded, the stop token. The child sees
// only erased_receiver, whose completions return {table->resume[i], record}.
struct table_base {
    inplace_stop_token (*stop_token)(void*) noexcept;
};
struct record {
    const table_base* table;
};
template <std::size_t N>
struct boundary_table : table_base {
    erased_handle (*resume[N])(void*) noexcept;
    void* (*destination[N])(void*) noexcept; // null for no payload
};

struct erased_env {
    record*                      k;
    TH_INLINE inplace_stop_token query(get_stop_token_t) const noexcept {
        return k->table->stop_token(k);
    }
};

// An optimization barrier: the record's address, as if from another TU.
template <class P>
TH_INLINE P opaque(P p) noexcept {
#if defined(__GNUC__)
    asm volatile("" : "+r"(p));
#endif
    return p;
}

// A boundary's table, made once its class is complete.
template <class Op>
inline constexpr auto table_of = Op::make_table();

template <class Sigs>
struct erased_receiver {
    record* k;
    using list                     = as_list_t<Sigs>;
    static constexpr std::size_t n = size_of<list>;

    TH_INLINE erased_env get_env() const noexcept { return {k}; }

    template <class Sig>
    void* destination() noexcept {
        return static_cast<const boundary_table<n>*>(k->table)
            ->destination[index_in<Sig, list>](k);
    }
    template <class Sig>
    TH_INLINE erased_handle complete() const noexcept {
        // returned, not called
        return {
            static_cast<const boundary_table<n>*>(k->table)->resume[index_in<Sig, list>],
            k};
    }
};

// The caller's side of a boundary: the record, its table, and the receiver.
// Whatever is on the other side sees only the record.
template <class Sigs, class R>
struct boundary_base : record {
    using list                     = as_list_t<Sigs>;
    static constexpr std::size_t n = size_of<list>;

    static boundary_base* self_of(void* k) noexcept {
        return static_cast<boundary_base*>(static_cast<record*>(k));
    }
    template <class Sig>
    static erased_handle resume(void* k) noexcept {
        return settle(self_of(k)->rcvr.template complete<Sig>());
    }
    template <class Sig>
    static void* dest(void* k) noexcept {
        return destination_of<R, Sig>(self_of(k)->rcvr);
    }
    static inplace_stop_token stop_token_of(void* k) noexcept {
        if constexpr (unstoppable<stop_token_of_t<R>>) {
            return {};
        } else {
            return get_stop_token(th::get_env(self_of(k)->rcvr));
        }
    }
    static constexpr boundary_table<n> make_table() {
        boundary_table<n> t{};
        t.stop_token = &stop_token_of;
        for_each_index<n>([&]<std::size_t I>() {
            using Sig   = nth_t<I, list>;
            t.resume[I] = &resume<Sig>;
            if constexpr (has_payload<Sig>) {
                t.destination[I] = &dest<Sig>;
            }
        });
        return t;
    }

    R rcvr;

    explicit boundary_base(R r) : record{&table_of<boundary_base>}, rcvr(std::move(r)) {}
    boundary_base(boundary_base&&) = delete;
};

// A boundary whose callee is in the same translation unit, behind an
// optimization barrier.
template <class Child, class R>
struct boundary_op : boundary_base<completions_of_t<Child, erased_env>, R> {
    using sigs       = completions_of_t<Child, erased_env>;
    using child_op_t = connect_result_t<Child, erased_receiver<sigs>>;

    child_op_t child;

    boundary_op(Child&& c, R r)
        : boundary_base<sigs, R>(std::move(r)),
          child(th::connect(
              std::move(c),
              erased_receiver<sigs>{opaque<record*>(static_cast<record*>(this))})) {}

    // The callee's start, as if called through an entry in another TU.
    [[gnu::noinline]] static erased_handle start_child(child_op_t* c) noexcept {
        return settle(th::start(*c));
    }
    TH_INLINE erased_handle start() & noexcept { return start_child(opaque(&child)); }
};

// A boundary whose callee is compiled separately. Callee declares its
// completion signatures, the size and alignment of its operation, and
// three entry points, defined in another translation unit:
//
//   static void          connect(void* frame, record* k, Args...) noexcept;
//   static erased_handle start(void* frame) noexcept;
//   static void          destroy(void* frame) noexcept;
//
// The caller provides the frame; the callee constructs its operation there,
// connected to an erased_receiver over the caller's record.
template <class Callee, class R>
struct remote_op : boundary_base<typename Callee::sigs, R> {
    alignas(Callee::align) std::byte frame[Callee::size];

    template <class... Args>
    remote_op(R r, Args... args) : boundary_base<typename Callee::sigs, R>(std::move(r)) {
        Callee::connect(frame, this, args...);
    }
    ~remote_op() { Callee::destroy(frame); }

    TH_INLINE erased_handle start() & noexcept { return Callee::start(frame); }
};

template <class Callee, class... Args>
struct remote_sender {
    std::tuple<Args...> args;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return typename Callee::sigs();
    }

    template <class R>
    remote_op<Callee, R> connect(R rcvr) && {
        return std::apply(
            [&](Args... as) { return remote_op<Callee, R>(std::move(rcvr), as...); },
            args);
    }
};
template <class Callee, class... Args>
remote_sender<Callee, Args...> remote(Args... args) {
    return {{args...}};
}

template <class Child>
struct boundary_sender {
    Child child;

    // What the child declares against the erased environment, whatever ours.
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completions_of_t<Child, erased_env>();
    }

    template <class R>
    boundary_op<Child, R> connect(R rcvr) && {
        return {std::move(child), std::move(rcvr)};
    }
};
template <class Child>
boundary_sender<Child> erase_boundary(Child c) {
    return {std::move(c)};
}

// ---- a root: where the asynchronous world ends --------------------------
template <class Sigs, class Token = never_stop_token>
struct root_state {
    using list = as_list_t<Sigs>;
    storage_for_t<list> storage;
    int                 which = -1; // which completion arrived
    Token               token;      // here, where the root's completions run

    root_state()             = default;
    root_state(root_state&&) = delete;
    ~root_state() {
        for_each_index<size_of<list>>([&]<std::size_t I>() {
            if (which == int(I)) {
                destroy_payload<nth_t<I, list>>(storage.address());
            }
        });
    }

    template <class Sig>
    bool has() const noexcept {
        if constexpr ((index_in<Sig, list>) < size_of<list>) {
            return which == int(index_in<Sig, list>);
        } else {
            return false;
        }
    }
    template <class Sig>
    payload_t<Sig>& get() noexcept {
        return *static_cast<payload_t<Sig>*>(storage.address());
    }
    bool stopped() const noexcept { return has<set_stopped_t()>(); }
    bool failed() const noexcept {
        bool f = false;
        for_each_index<size_of<list>>([&]<std::size_t I>() {
            f = f || (which == int(I) && same_as<tag_of_t<nth_t<I, list>>, set_error_t>);
        });
        return f;
    }
};

// Move-only, as the standard allows, so that nothing here can copy a
// receiver.
template <class Sigs, class Token = never_stop_token>
struct root_rcvr : operation_handle<root_rcvr<Sigs, Token>, root_state<Sigs, Token>> {
    using state_t = root_state<Sigs, Token>;
    using base_t  = operation_handle<root_rcvr, state_t>;

    explicit root_rcvr(state_t* s) noexcept : base_t(s) {}
    root_rcvr(root_rcvr&&) noexcept = default;
    root_rcvr(const root_rcvr&)     = delete;

    TH_INLINE auto get_env() const noexcept {
        if constexpr (unstoppable<Token>) {
            return empty_env{};
        } else {
            return prop<get_stop_token_t, Token>{this->op()->token};
        }
    }

    template <class Sig>
    void* destination() noexcept {
        return this->op()->storage.address();
    }

  private:
    friend base_t;

    template <class Sig>
    TH_INLINE static no_operation resume(state_t& self) noexcept {
        self.which = int(index_in<Sig, typename state_t::list>);
        return {};
    }
};

template <class Token>
using root_env_t =
    std::conditional_t<unstoppable<Token>, empty_env, prop<get_stop_token_t, Token>>;

// Connect s to a root, run it to completion on this thread, then call
// use(state). Everything here completes inline; nothing waits. The root's
// environment has a stop token if one is given.
template <class S, class Use, class Token = never_stop_token>
void run_root(S&& s, Use use, Token token = {}) {
    using Sigs = completions_of_t<S, root_env_t<Token>>;
    root_state<Sigs, Token> st;
    st.token = token;
    auto op  = th::connect(std::forward<S>(s), root_rcvr<Sigs, Token>{&st});
    th::run(th::start(op));
    use(st);
}

// ---- bulk -----------------------------------------------------------------
// For a value completion, calls f(i, args...) for each i in [0, shape), with
// lvalues of the values, then completes with them. The values are where the
// receiver wants them from the start: the child constructs them at the
// receiver's destination, f works on them there, and nothing is moved. The
// execution policy is accepted and ignored: the calls are made in order, on
// the completing thread.
// Whether f(i, args...) can't throw, with lvalues of a value completion's
// arguments.
template <class F, class Shape, class Sig>
inline constexpr bool bulk_nothrow = false;
template <class F, class Shape, class... As>
inline constexpr bool bulk_nothrow<F, Shape, set_value_t(As...)> =
    std::is_nothrow_invocable_v<F&, Shape, std::remove_reference_t<As>&...>;

template <class Child, class Shape, class F, class R>
struct bulk_op {
    using child_sigs = completions_of_t<Child, env_of_t<R>>;

    struct rcvr_t : operation_handle<rcvr_t, bulk_op> {
        using base_t = operation_handle<rcvr_t, bulk_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            return destination_of<R, Sig>(this->op()->rcvr);
        }
        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(bulk_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_value_t>) {
                return self.template run<Sig>();
            } else {
                return later<Sig>(self.rcvr);
            }
        }
    };

    Shape                           shape;
    F                               f;
    R                               rcvr;
    connect_result_t<Child, rcvr_t> child;

    bulk_op(Child&& c, Shape s, F fn, R r)
        : shape(s),
          f(std::move(fn)),
          rcvr(std::move(r)),
          child(th::connect(std::move(c), rcvr_t(this))) {}
    bulk_op(bulk_op&&) = delete;

    TH_INLINE auto start() & noexcept { return th::start(child); }

    template <class Sig>
    TH_INLINE auto run() noexcept {
        void* p     = destination_of<R, Sig>(rcvr);
        auto  calls = [&] {
            apply_payload<Sig>(p, [&](auto&&... as) {
                for (Shape i = 0; i < shape; ++i) {
                    f(i, as...);
                }
            });
        };
        if constexpr (bulk_nothrow<F, Shape, Sig>) {
            calls();
            return later<Sig>(rcvr);
        } else {
            bool ok = true;
            try {
                calls();
            } catch (...) {
                destroy_payload<Sig>(p); // before the error takes its place
                ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
                    exception_ptr(std::current_exception());
                ok = false;
            }
            return either(
                ok,
                [&] { return later<Sig>(rcvr); },
                [&] { return later<set_error_t(exception_ptr)>(rcvr); });
        }
    }
};

template <class Child, class Shape, class F>
struct bulk_sender {
    Child child;
    Shape shape;
    F     f;

    template <class Env>
    static consteval auto get_completion_signatures() {
        using sigs             = completions_of_t<Child, Env>;
        constexpr bool nothrow = []<class... S>(type_list<S...>) {
            return (bulk_nothrow<F, Shape, S> && ...);
        }(select_t<set_value_t, sigs>());
        return concat_t<sigs, eptr_unless<nothrow>>();
    }

    template <class R>
    bulk_op<Child, Shape, F, R> connect(R rcvr) && {
        return {std::move(child), shape, std::move(f), std::move(rcvr)};
    }
};
template <class Child, class Policy, class Shape, class F>
bulk_sender<Child, Shape, F> bulk(Child c, Policy&&, Shape shape, F f) {
    return {std::move(c), shape, std::move(f)};
}

// ---- into_variant ---------------------------------------------------------
// Turns the child's value completions into one, set_value_t(V), where V is a
// variant with a tuple of decayed values per value completion. The child's
// values are staged here, then the variant is constructed in place at the
// receiver's destination.
template <class Sig>
struct values_tuple;
template <class... As>
struct values_tuple<set_value_t(As...)> {
    using type                    = std::tuple<std::decay_t<As>...>;
    static constexpr bool nothrow = std::is_nothrow_constructible_v<type, As...>;
};

template <class List>
struct variant_of;
template <class... S>
struct variant_of<type_list<S...>> {
    using type                    = std::variant<typename values_tuple<S>::type...>;
    static constexpr bool nothrow = (values_tuple<S>::nothrow && ...);
};

template <class Child, class R>
struct into_variant_op {
    using child_sigs = completions_of_t<Child, env_of_t<R>>;
    using value_list = select_t<set_value_t, child_sigs>;
    using variant_t  = typename variant_of<value_list>::type;
    using out_sig    = set_value_t(variant_t);

    struct rcvr_t : operation_handle<rcvr_t, into_variant_op> {
        using base_t = operation_handle<rcvr_t, into_variant_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_value_t>) {
                return this->op()->staged.address();
            } else {
                return destination_of<R, Sig>(this->op()->rcvr);
            }
        }
        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(into_variant_op& self) noexcept {
            if constexpr (same_as<tag_of_t<Sig>, set_value_t>) {
                return self.template wrap<Sig>();
            } else {
                return later<Sig>(self.rcvr);
            }
        }
    };

    R                               rcvr;
    storage_for_t<value_list>       staged;
    connect_result_t<Child, rcvr_t> child;

    into_variant_op(Child&& c, R r)
        : rcvr(std::move(r)), child(th::connect(std::move(c), rcvr_t(this))) {}
    into_variant_op(into_variant_op&&) = delete;

    TH_INLINE auto start() & noexcept { return th::start(child); }

    template <class Sig>
    TH_INLINE auto wrap() noexcept {
        constexpr std::size_t I    = index_in<Sig, value_list>;
        void*                 p    = staged.address();
        auto                  make = [&] {
            apply_payload<Sig>(p, [&](auto&&... as) {
                ::new (destination_of<R, out_sig>(rcvr))
                    variant_t(std::in_place_index<I>, static_cast<decltype(as)&&>(as)...);
            });
        };
        if constexpr (values_tuple<Sig>::nothrow) {
            make();
            destroy_payload<Sig>(p);
            return later<out_sig>(rcvr);
        } else {
            bool ok = true;
            try {
                make();
            } catch (...) {
                ::new (destination_of<R, set_error_t(exception_ptr)>(rcvr))
                    exception_ptr(std::current_exception());
                ok = false;
            }
            destroy_payload<Sig>(p);
            return either(
                ok,
                [&] { return later<out_sig>(rcvr); },
                [&] { return later<set_error_t(exception_ptr)>(rcvr); });
        }
    }
};

template <class Child>
struct into_variant_sender {
    Child child;

    template <class Env>
    static consteval auto get_completion_signatures() {
        using sigs   = completions_of_t<Child, Env>;
        using values = select_t<set_value_t, sigs>;
        static_assert(size_of<values> > 0, "into_variant needs a value completion");
        using V = variant_of<values>;
        return concat_t<completion_signatures<set_value_t(typename V::type)>,
                        to_sigs_t<select_t<set_error_t, sigs>>,
                        to_sigs_t<select_t<set_stopped_t, sigs>>,
                        eptr_unless<V::nothrow>>();
    }

    template <class R>
    into_variant_op<Child, R> connect(R rcvr) && {
        return {std::move(child), std::move(rcvr)};
    }
};
template <class Child>
into_variant_sender<Child> into_variant(Child c) {
    return {std::move(c)};
}

} // namespace th

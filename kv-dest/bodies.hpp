// The bodies, hand-lowered once each and generic over the variant. Included
// by stores.cpp (the erased definitions) and by bench.cpp (the statically
// composed `or_else` above an erased call).
#pragma once

#include "proto.hpp"

// ---- what a frame knows of its parent ---------------------------------------
// Through the protocol: what the entry was passed.
template <class V, class T>
struct link {
    [[no_unique_address]] typename V::template dst<T> d;
    typename V::rec*                                  k;
    template <int I>
    void* dest() const noexcept {
        return V::template dest<I, T>(d, k);
    }
};

// Row 5 in statically composed code: the parent's storage is a type the
// child knows, so "where do I construct completion I?" is answered at compile
// time, relative to one pointer.
template <class V, class T>
struct static_link {
    slot<T>*         s;
    typename V::rec* k;
    template <int I>
    void* dest() const noexcept {
        if constexpr (I == VAL)
            return &s->value;
        else
            return &s->error;
    }
};

// The destinations a frame hands its child when it passes every completion
// through: rows 1, 2 and 5 forward what they were given; rows 3 and 4 hand
// over their own record, which the frame's `wire` filled.
template <class V, class Up>
auto pass_dst(const Up& up) noexcept {
    if constexpr (by_record<V>)
        return nodst{};
    else
        return up.d;
}

struct none_t {};
union own_err {
    std::exception_ptr error;
    own_err() noexcept {}
    ~own_err() {}
};

// ============================================================================
// The backing store: `get` (an optional; never fails) and `fetch` (a string,
// or an error when the key is absent). Inline, or from the event loop.
// ============================================================================
template <class V, class Up>
[[gnu::always_inline]] inline typename V::handle
get_now(const mem_data& self, const str& key, Up up) noexcept {
    // co_return of a prvalue, in place
    ::new (up.template dest<VAL>()) value_t(self.find(key));
    return V::template complete<VAL>(up.k);
}

template <class V, class Up>
[[gnu::always_inline]] inline typename V::handle
fetch_now(const mem_data& self, const str& key, Up up) noexcept {
    if (const str* v = self.lookup(key)) {
        ::new (up.template dest<VAL>()) str(*v);
        return V::template complete<VAL>(up.k);
    }
    ::new (up.template dest<ERR>()) std::exception_ptr(g_not_found);
    return V::template complete<ERR>(up.k);
}

template <class V, class T, bool Fetch>
struct leaf_frame : task {
    link<V, T>      up;
    const mem_data* self;
    str*            key; // the caller's parameter object: valid until we complete
};

template <class V, class T, bool Fetch>
[[gnu::always_inline]] inline typename V::handle leaf_body(const mem_data& self,
                                                           str&            key,
                                                           typename V::template dst<T> d,
                                                           typename V::rec*            k,
                                                           async_sp sp) noexcept {
    link<V, T> up{d, k};
    if (!self.deferred) {
        if constexpr (Fetch)
            return fetch_now<V>(self, key, up);
        else
            return get_now<V>(self, key, up);
    }
    using F      = leaf_frame<V, T, Fetch>;
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(F))) F;
    f->up        = up;
    f->self      = &self;
    f->key       = &key;
    f->run       = [](task* t) noexcept {
        auto*              f  = static_cast<F*>(t);
        link<V, T>         up = f->up;
        typename V::handle h;
        if constexpr (Fetch)
            h = fetch_now<V>(*f->self, *f->key, up);
        else
            h = get_now<V>(*f->self, *f->key, up);
        async_sp top;
        async_pop(&top, f); // the frame is gone before the caller continues
        V::run(h);
    };
    defer(f);
    return V::none();
}

// ============================================================================
// The cache (W1). It passes every completion through, and reads the value it
// passed through to fill the cache:
//
//   value_t cache::get(str key) {
//     if (auto hit = entries_.find(key)) co_return hit;
//     auto value = co_await backing_->get(key);
//     if (value) entries_.insert(key, *value);
//     co_return value;
//   }
// ============================================================================
template <class V>
struct cache_frame : V::rec {
    link<V, value_t> up;
    cache_entries*   entries;
    str*             key;
    // the callee's parameter object: a temporary of the full-expression
    union {
        str arg;
    };
    cache_frame() noexcept {}
    ~cache_frame() {}

    template <int I>
    void* my_dest() noexcept {
        return up.template dest<I>();
    }
    template <int I>
    typename V::handle after() noexcept {
        arg.~str();
        if constexpr (I == VAL) {
            value_t& v = *static_cast<value_t*>(up.template dest<VAL>());
            if (v)
                entries->insert(*key, *v);
        }
        auto     k = up.k;
        async_sp top;
        async_pop(&top, this);
        return V::template complete<I>(k);
    }
};

template <class V, class Store>
[[gnu::always_inline]] inline typename V::handle
cache_body(cache_entries&                    entries,
           const Store&                      backing,
           str&                              key,
           typename V::template dst<value_t> d,
           typename V::rec*                  k,
           async_sp                          sp) noexcept {
    link<V, value_t> up{d, k};
    // `hit` is the return object: it's constructed in the caller's storage
    value_t& hit = *::new (up.template dest<VAL>()) value_t(entries.find(key));
    if (hit)
        return V::template complete<VAL>(k);
    hit.~value_t();
    using F      = cache_frame<V>;
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(F))) F;
    f->up        = up;
    f->entries   = &entries;
    f->key       = &key;
    ::new (&f->arg) str(key);
    V::wire(f);
    auto n = backing.get(f->arg, pass_dst<V>(f->up), f, top);
    return V::fall_through(n, f);
}

// ============================================================================
// A middle that filters (W2, W3; Robert Leahy's case): the value passes
// through, an error becomes the fallback.
//
//   str get_or(str key, str fallback) {
//     try { co_return co_await backing_->fetch(key); }
//     catch (...) { co_return fallback; }
//   }
//
// T is the value type, Fb what T is made from on the error path.
// ============================================================================
template <class V, class Up, class T, class Fb>
struct filter_frame : V::rec {
    Up        up;
    const Fb* fb;
    // the callee's parameter object
    union {
        str arg;
    };
    // what the middle keeps of its own: in row 1 the child's whole slot (the
    // value has to be moved out of it); elsewhere room for the error alone
    std::conditional_t<V::row == 1, slot<T>, own_err> own;
    // the child's table
    [[no_unique_address]] std::conditional_t<by_table<V>, ptab, none_t> tab;
    filter_frame() noexcept {}
    ~filter_frame() {}

    template <int I>
    void* my_dest() noexcept {
        if constexpr (I == VAL)
            return up.template dest<VAL>(); // passed through
        else
            return &own.error; // filtered
    }
    template <int I>
    typename V::handle after() noexcept {
        arg.~str();
        if constexpr (I == VAL) {
            if constexpr (V::row == 1) {
                ::new (up.template dest<VAL>()) T(std::move(own.value));
                own.value.~T();
            }
        } else if constexpr (I == ERR) {
            own.error.~exception_ptr(); // the handler doesn't name it
            ::new (up.template dest<VAL>()) T(*fb);
        }
        auto     k = up.k;
        async_sp top;
        async_pop(&top, this);
        return V::template complete<I == ERR ? VAL : I>(k);
    }
};

// The destinations a filtering frame hands its child.
template <class V, class F>
[[gnu::always_inline]] inline auto filter_dst(F* f) noexcept {
    if constexpr (V::row == 1)
        return &f->own;
    else if constexpr (by_record<V>)
        return nodst{};
    else {
        f->tab.p[VAL] = f->template my_dest<VAL>();
        f->tab.p[ERR] = f->template my_dest<ERR>();
        return &f->tab;
    }
}

// `child(arg, dst, k, top)` starts the awaited operation.
template <class V, class T, class Up, class Fb, class Child>
[[gnu::always_inline]] inline typename V::handle
filter_body(Up up, const Fb& fb, const str& key, Child child, async_sp sp) noexcept {
    using F      = filter_frame<V, Up, T, Fb>;
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(F))) F;
    f->up        = up;
    f->fb        = &fb;
    ::new (&f->arg) str(key);
    V::wire(f);
    auto n = child(f->arg, filter_dst<V>(f), static_cast<typename V::rec*>(f), top);
    return V::fall_through(n, f);
}

// ============================================================================
// A middle that observes and passes everything through (W4):
//
//   str pass::get_or(str key, str fallback) {
//     // as a scope_exit
//     auto r = co_await next_->get_or(std::move(key), std::move(fallback));
//     ++count_;
//     co_return r;
//   }
//
// (`++count_` runs on every completion; the result goes straight through.)
// ============================================================================
template <class V>
struct pass_frame : V::rec {
    link<V, str> up;
    long*        count;
    union {
        str arg;
    };
    union {
        str fbarg;
    };
    pass_frame() noexcept {}
    ~pass_frame() {}

    template <int I>
    void* my_dest() noexcept {
        return up.template dest<I>();
    }
    template <int I>
    typename V::handle after() noexcept {
        arg.~str();
        fbarg.~str();
        ++*count;
        auto     k = up.k;
        async_sp top;
        async_pop(&top, this);
        return V::template complete<I>(k);
    }
};

template <class V, class Store>
[[gnu::always_inline]] inline typename V::handle
pass_body(long&                         count,
          const Store&                  next,
          str&                          key,
          str&                          fb,
          typename V::template dst<str> d,
          typename V::rec*              k,
          async_sp                      sp) noexcept {
    using F      = pass_frame<V>;
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(F))) F;
    f->up        = {d, k};
    f->count     = &count;
    ::new (&f->arg) str(std::move(key));
    ::new (&f->fbarg) str(std::move(fb));
    V::wire(f);
    auto n = next.get_or(f->arg, f->fbarg, pass_dst<V>(f->up), f, top);
    return V::fall_through(n, f);
}

// ============================================================================
// The interface: every function is reached by a virtual call, the entry
// visible (kv-handle's fastest column), so that the variant is the only
// difference.
// ============================================================================
template <class V>
struct store {
    using H = typename V::handle;
    using R = typename V::rec;
    template <class T>
    using D          = typename V::template dst<T>;
    virtual ~store() = default;
    virtual H       get(str& key, D<value_t> d, R* k, async_sp sp) const noexcept;
    virtual H       fetch(str& key, D<str> d, R* k, async_sp sp) const noexcept;
    virtual H       get_or(str& key, str& fb, D<str> d, R* k, async_sp sp) const noexcept;
    virtual value_t get_sync(str key) const;
    virtual str     get_or_sync(str key, str fb) const;
    virtual long    count() const noexcept { return 0; }
};

// kinds: 'm' backing store, 'c' cache over a backing store, 'g' get_or over a
// backing store, 'p' pass over a get_or
template <class V>
store<V>* make_store(char kind, const store<V>* below, bool retain, bool deferred);

#define DECLARE_EXTERN(V) extern template struct store<V>;
DECLARE_EXTERN(V1A)
DECLARE_EXTERN(V2A)
DECLARE_EXTERN(V3A)
DECLARE_EXTERN(V4A)
DECLARE_EXTERN(V5A)
DECLARE_EXTERN(V1B)
DECLARE_EXTERN(V2B)
DECLARE_EXTERN(V3B)
DECLARE_EXTERN(V4B)
DECLARE_EXTERN(V5B)
#undef DECLARE_EXTERN

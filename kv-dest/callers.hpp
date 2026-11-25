// The callers, shared by bench.cpp and checks.cpp. They see only bodies.hpp.
#pragma once

#include "bodies.hpp"

// ---- the root of a call tree: the lowered form of `T r = co_await f(...);` --
template <class V, class T>
struct root : V::rec {
    slot<T> out;
    int     how = PENDING;
    root() noexcept { V::wire(this); }
    root(const root&) = delete;
    ~root() { out.destroy(how); }

    template <int I>
    void* my_dest() noexcept {
        if constexpr (I == VAL)
            return &out.value;
        else
            return &out.error;
    }
    template <int I>
    typename V::handle after() noexcept {
        how = I;
        return V::none();
    }
    // What an erased callee is passed for the destinations. Row 5 meets an
    // erased call here, so it builds row 2's table.
    typename V::template dst<T> dst(ptab& t) noexcept {
        if constexpr (V::row == 1)
            return &out;
        else if constexpr (by_record<V>)
            return nodst{};
        else {
            t.p[VAL] = &out.value;
            t.p[ERR] = &out.error;
            return &t;
        }
    }
    // This caller is the bottom of the machine stack.
    void drive(typename V::handle n) noexcept {
        n = V::fall_through(n, this);
        V::run(n);
        while (how == PENDING)
            run_pending();
    }
};

// ---- W1: value_t v = co_await cache.get(key); --------------------------------
template <class V>
[[gnu::noinline]] std::size_t call_get(const store<V>& s, const str& key, async_sp sp) {
    str              arg(key); // the parameter object, in the caller
    root<V, value_t> r;
    ptab             t;
    r.drive(s.get(arg, r.dst(t), &r, sp));
    return (r.how == VAL && r.out.value) ? 1 : 0;
}

// ---- W2, W4: str v = co_await s.get_or(key, fb); -----------------------------
template <class V>
[[gnu::noinline]] std::size_t
call_get_or(const store<V>& s, const str& key, const str& fb, async_sp sp) {
    str          arg(key), fbarg(fb);
    root<V, str> r;
    ptab         t;
    r.drive(s.get_or(arg, fbarg, r.dst(t), &r, sp));
    return r.how == VAL ? r.out.value.size() : 0;
}

// ---- W3: str v = co_await or_else(m.fetch(key), fb); -------------------------
// `or_else` is statically composed with this caller: the filtering middle of
// W2, but a template the caller instantiates, above an erased `fetch`. Rows 1
// to 4 reach the caller's storage through the protocol; row 5 through a type
// the middle knows (`static_link`).
template <class V>
[[gnu::noinline]] std::size_t
call_or_else(const store<V>& m, const str& key, const str& fb, async_sp sp) {
    root<V, str> r;
    auto child = [&m](str& arg, auto cd, typename V::rec* ck, async_sp top) noexcept {
        return m.fetch(arg, cd, ck, top);
    };
    ptab t; // rows 2 and 5 only: lives as long as the root, as a parent's table must
    typename V::handle n;
    if constexpr (V::row == 5)
        n = filter_body<V, str>(static_link<V, str>{&r.out, &r}, fb, key, child, sp);
    else
        n = filter_body<V, str>(link<V, str>{r.dst(t), &r}, fb, key, child, sp);
    r.drive(n);
    return r.how == VAL ? r.out.value.size() : 0;
}

// ---- the yardsticks ----------------------------------------------------------
template <class V>
[[gnu::noinline]] std::size_t call_get_sync(const store<V>& s, const str& key) {
    value_t v = s.get_sync(key);
    return v ? 1 : 0;
}
template <class V>
[[gnu::noinline]] std::size_t
call_get_or_sync(const store<V>& s, const str& key, const str& fb) {
    return s.get_or_sync(key, fb).size();
}

// ---- workloads ----------------------------------------------------------------
enum workload : int {
    w_get_hit,
    w_get_miss,
    w_get_or_value,
    w_get_or_error,
    w_or_else_value,
    w_or_else_error,
    w_pass2_value,
    w_pass2_error,
    n_workloads
};
inline const char* const workload_names[] = {"get, hit",
                                             "get, miss",
                                             "get_or, value",
                                             "get_or, error",
                                             "or_else, value",
                                             "or_else, error",
                                             "2 x pass, value",
                                             "2 x pass, error"};
inline bool              error_path(int w) {
    return w == w_get_or_error || w == w_or_else_error || w == w_pass2_error;
}
inline std::size_t expected(int w) {
    if (w <= w_get_miss)
        return 1;
    return error_path(w) ? 8 : 2; // "fallback" or "v1"
}

struct rig_base {
    virtual ~rig_base()               = default;
    virtual std::size_t call()        = 0;
    virtual long        count() const = 0;
};

template <class V>
struct rig final : rig_base {
    int       w;
    bool      sync;
    store<V>* m;
    store<V>* top;
    async_sp  sp;
    str       key, fb;
    rig(int w_, bool deferred, bool sync_)
        : w(w_), sync(sync_), sp(async_stack_create()) {
        m = make_store<V>('m', nullptr, false, deferred);
        switch (w) {
        case w_get_hit:
            top = make_store<V>('c', m, true, deferred);
            break;
        case w_get_miss:
            top = make_store<V>('c', m, false, deferred);
            break;
        case w_get_or_value:
        case w_get_or_error:
            top = make_store<V>('g', m, false, deferred);
            break;
        case w_or_else_value:
        case w_or_else_error:
            top = m;
            break;
        default:
            top = make_store<V>(
                'p',
                make_store<V>(
                    'p', make_store<V>('g', m, false, deferred), false, deferred),
                false,
                deferred);
        }
        key = error_path(w) ? "kx" : "k1";
        fb  = "fallback";
    }
    std::size_t call() override {
        if (sync) {
            if (w <= w_get_miss)
                return call_get_sync<V>(*top, key);
            return call_get_or_sync<V>(*top, key, fb);
        }
        switch (w) {
        case w_get_hit:
        case w_get_miss:
            return call_get<V>(*top, key, sp);
        case w_or_else_value:
        case w_or_else_error:
            return call_or_else<V>(*m, key, fb, sp);
        default:
            return call_get_or<V>(*top, key, fb, sp);
        }
    }
    long count() const override { return top->count(); }
};

// variant index 0..9 in all_variants order; the yardstick is variant -1
template <class... Vs>
inline rig_base* make_rig_from(tlist<Vs...>, int v, int w, bool deferred) {
    rig_base* r = nullptr;
    int       i = 0;
    ((r = (i++ == v ? static_cast<rig_base*>(new rig<Vs>(w, deferred, false)) : r)), ...);
    return r;
}
inline rig_base* make_rig(int v, int w, bool deferred) {
    if (v < 0)
        return new rig<V1A>(w, false, true);
    return make_rig_from(all_variants{}, v, w, deferred);
}
template <class... Vs>
inline const char* variant_name_from(tlist<Vs...>, int v) {
    const char* names[] = {Vs::name...};
    return names[v];
}
inline const char* variant_name(int v) {
    return v < 0 ? "synchronous" : variant_name_from(all_variants{}, v);
}

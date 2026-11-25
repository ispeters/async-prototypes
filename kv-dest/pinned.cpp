// An immovable value type through the filtering middle, one variant per
// build: g++ -std=c++20 -DVARIANT=V2A pinned.cpp async_abi.o
//
//   pinned get_or(str key, int fallback) {
//     try { co_return co_await leaf(key); } catch (...) { co_return pinned(fallback); }
//   }
#include "bodies.hpp"

#include <cstdio>

struct pinned {
    int v;
    explicit pinned(int x) noexcept : v(x) {}
    pinned(pinned&&) = delete;
};

// The awaited operation: constructs a `pinned` in place, or fails.
template <class V>
typename V::handle
leaf(str& key, typename V::template dst<pinned> d, typename V::rec* k) noexcept {
    link<V, pinned> up{d, k};
    if (key == str("k1")) {
        ::new (up.template dest<VAL>()) pinned(42);
        return V::template complete<VAL>(k);
    }
    ::new (up.template dest<ERR>()) std::exception_ptr(g_not_found);
    return V::template complete<ERR>(k);
}

template <class V>
struct proot : V::rec {
    slot<pinned> out;
    int          how = PENDING;
    proot() noexcept { V::wire(this); }
    ~proot() {
        if (how == VAL) {
            out.value.~pinned();
        }
    }
    template <int I>
    void* my_dest() noexcept {
        if constexpr (I == VAL) {
            return &out.value;
        } else {
            return &out.error;
        }
    }
    template <int I>
    typename V::handle after() noexcept {
        how = I;
        return V::none();
    }
};

template <class V>
int run(const char* key) {
    proot<V> r;
    ptab     t;
    int      fb    = 7;
    auto     child = [](str& arg, auto cd, typename V::rec* ck, async_sp) noexcept {
        return leaf<V>(arg, cd, ck);
    };
    async_sp           sp = async_stack_create();
    typename V::handle n;
    if constexpr (V::row == 1) {
        n = filter_body<V, pinned>(link<V, pinned>{&r.out, &r}, fb, str(key), child, sp);
    } else if constexpr (V::row == 5) {
        n = filter_body<V, pinned>(
            static_link<V, pinned>{&r.out, &r}, fb, str(key), child, sp);
    } else if constexpr (by_record<V>) {
        n = filter_body<V, pinned>(link<V, pinned>{nodst{}, &r}, fb, str(key), child, sp);
    } else {
        t.p[VAL] = &r.out.value;
        t.p[ERR] = &r.out.error;
        n = filter_body<V, pinned>(link<V, pinned>{&t, &r}, fb, str(key), child, sp);
    }
    V::run(V::fall_through(n, &r));
    async_stack_destroy(sp);
    return r.how == VAL ? r.out.value.v : -1;
}

int main() {
    int a = run<VARIANT>("k1"), b = run<VARIANT>("kx");
    std::printf("%s: value path %d, error path %d\n", VARIANT::name, a, b);
    return !(a == 42 && b == 7);
}

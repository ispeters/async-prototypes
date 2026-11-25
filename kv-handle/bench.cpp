// The callers, and the measurements. This file sees only kv_store.hpp.
#include "kv_store.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- heap accounting --------------------------------------------------------
static long g_news, g_new_bytes;
#ifdef COUNT_HEAP
void* operator new(std::size_t n) {
    ++g_news;
    g_new_bytes += n;
    if (void* p = std::malloc(n))
        return p;
    std::abort();
}
void* operator new(std::size_t n, std::align_val_t a) {
    ++g_news;
    g_new_bytes += n;
    std::size_t al = static_cast<std::size_t>(a);
    if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al))
        return p;
    std::abort();
}
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#endif

static std::size_t g_caller_frame;

// ---- caller: synchronous ----------------------------------------------------
[[gnu::noinline]] std::size_t call_sync(const kv_store& s, const str& key) {
    value_t v = s.get_sync(key);
    return v ? 1 : 0;
}

// ---- caller: a C++26 receiver -----------------------------------------------
// The lowered form of `value_t v = co_await s.get(key);` at the root of a call
// tree, as it is today, driven to completion. `Store` is kv_store or
// kv_store_h: either `function` connects to this receiver.
struct root_state {
    slot<value_t> out;
    int           how = ASYNC_PENDING;
};

struct root_rcvr_base {
    using receiver_concept = ex::receiver_tag;
    root_state* st;
    void        set_value(value_t&& v) && noexcept {
        ::new (&st->out.value) value_t(std::move(v));
        st->how = ASYNC_VALUE;
    }
    void set_error(std::exception_ptr e) && noexcept {
        ::new (&st->out.error) std::exception_ptr(std::move(e));
        st->how = ASYNC_ERROR;
    }
    void set_stopped() && noexcept { st->how = ASYNC_STOPPED; }
};

// offers a strand of the async stack, as the frame allocator and as the cursor
struct root_rcvr_strand : root_rcvr_base {
    async_sp*  top;
    strand_env get_env() const noexcept { return {top}; }
};

// no frame allocator in the environment
struct root_rcvr_plain : root_rcvr_base {
    ex::env<> get_env() const noexcept { return {}; }
};

template <class Store, class Rcvr, class... Extra>
[[gnu::noinline]] std::size_t call_cxx26(const Store& s, const str& key, Extra... extra) {
    root_state st;
    {
        auto op        = ex::connect(s.get(key), Rcvr{{&st}, extra...});
        g_caller_frame = sizeof(op) + sizeof(st);
        ex::start(op);
        while (st.how == ASYNC_PENDING)
            async_run_pending();
    }
    std::size_t r = (st.how == ASYNC_VALUE && st.out.value) ? 1 : 0;
    st.out.destroy(st.how);
    return r;
}

// ---- caller: a handle-aware receiver ----------------------------------------
struct root_cont : async_cont {
    int how = ASYNC_PENDING;
    root_cont() {
        resume = [](async_cont* c, int h) noexcept -> async_next {
            static_cast<root_cont*>(c)->how = h;
            return {nullptr, ASYNC_PENDING};
        };
    }
    // this caller is the bottom of the stack
    void drive(async_next n) noexcept {
        if (n.next == this)
            how = n.how;
        else
            async_run(n);
        while (how == ASYNC_PENDING)
            async_run_pending();
    }
};

// the same await as make-the-sender, connect, start
[[gnu::noinline]] std::size_t
call_hidden(const kv_store_h& s, const str& key, async_sp sp) {
    slot<value_t> out;
    root_cont     k;
    {
        hget_fn fn     = s.get(key);
        g_caller_frame = sizeof(fn) + sizeof(out) + sizeof(k);
        k.drive(fn.connect(handle_rcvr<value_t>{&out, &k, sp}).start());
    }
    std::size_t r = (k.how == ASYNC_VALUE && out.value) ? 1 : 0;
    out.destroy(k.how);
    return r;
}

// the same await as one call
[[gnu::noinline]] std::size_t
call_visible(const kv_store_v& s, const str& key, async_sp sp) {
    str           arg(key); // the parameter object, in the caller
    slot<value_t> out;      // the result, in the caller
    root_cont     k;
    g_caller_frame = sizeof(arg) + sizeof(out) + sizeof(k);
    k.drive(s.get_entry(arg, &out, &k, sp));
    std::size_t r = (k.how == ASYNC_VALUE && out.value) ? 1 : 0;
    out.destroy(k.how);
    return r;
}

// ---- driver -----------------------------------------------------------------
struct config {
    const char* name;
    char root; // 's' synchronous; 'c' C++26 receiver with a strand as frame allocator;
               // 'd' C++26 receiver with no frame allocator; 'h' handle-aware receiver
    char cache_kind, mem_kind;
};

static const config configs[] = {
    {"synchronous", 's', 'p', 'p'},                        // 0
    {"published", 'c', 'p', 'p'},                          // 1
    {"lowered to a C++26 sender", 'c', 'l', 'l'},          // 2
    {"hidden entry", 'h', 'n', 'n'},                       // 3
    {"hidden entry, owning factory", 'h', 'o', 'o'},       // 4
    {"visible entry", 'h', 'v', 'v'},                      // 5
    {"compat: new cache, today's store", 'h', 'n', 'p'},   // 6
    {"compat: today's cache, new store", 'c', 'p', 'n'},   // 7
    {"compat: C++26 root, strand; new", 'c', 'n', 'n'},    // 8
    {"compat: C++26 root, no stack; new", 'd', 'n', 'n'},  // 9
    {"compat: published code on hfn", 'h', 'w', 'w'},      // 10
    {"compat: same, C++26 root, strand", 'c', 'w', 'w'},   // 11
    {"published, forwards allocator", 'c', 'a', 'a'},      // 12
    {"compat: 12's cache, new store", 'c', 'a', 'n'},      // 13
    {"published, forwards cursor", 'c', 'c', 'c'},         // 14
    {"compat: 14's cache, new store", 'c', 'c', 'n'},      // 15
    {"compat: same, C++26 root, no stack", 'd', 'c', 'n'}, // 16
};

static std::size_t call(const config& c, const stores& s, const str& key, async_sp& sp) {
    switch (c.root) {
    case 's':
        return call_sync(*s.today, key);
    case 'c':
        if (s.alloc)
            return call_cxx26<kv_store_alloc, root_rcvr_strand, async_sp*>(
                *s.alloc, key, &sp);
        if (s.cursor)
            return call_cxx26<kv_store_cursor, root_rcvr_strand, async_sp*>(
                *s.cursor, key, &sp);
        return s.today
                   ? call_cxx26<kv_store, root_rcvr_strand, async_sp*>(*s.today, key, &sp)
                   : call_cxx26<kv_store_h, root_rcvr_strand, async_sp*>(
                         *s.hidden, key, &sp);
    case 'd':
        if (s.cursor)
            return call_cxx26<kv_store_cursor, root_rcvr_plain>(*s.cursor, key);
        return s.today ? call_cxx26<kv_store, root_rcvr_plain>(*s.today, key)
                       : call_cxx26<kv_store_h, root_rcvr_plain>(*s.hidden, key);
    default:
        return s.hidden ? call_hidden(*s.hidden, key, sp)
                        : call_visible(*s.visible, key, sp);
    }
}

#ifdef COUNT_MOVES
static void counts(const config& c, const char* what, bool retain, bool deferred) {
    stores   s  = make_stores(c.cache_kind, c.mem_kind, retain, deferred);
    async_sp sp = async_stack_create();
    str      key("k1");
    if (retain)
        call(c, s, key, sp); // warm the cache, so the measured call is a hit
    str::key_copies = str::key_moves = str::val_copies = str::val_moves = 0;
    g_news = g_new_bytes  = 0;
    async_high_water      = sizeof(async_slab);
    async_slabs_allocated = 0;
    g_caller_frame        = 0;
    std::size_t found     = call(c, s, key, sp);
    if (!found)
        std::abort();
    std::printf(
        "%-36s| %-14s| %ld / %ld | %ld / %ld | %ld (%ld B) | %zu | %zu B | %zu B\n",
        c.name,
        what,
        str::key_copies,
        str::key_moves,
        str::val_copies,
        str::val_moves,
        g_news,
        g_new_bytes,
        async_slabs_allocated,
        async_high_water - sizeof(async_slab),
        g_caller_frame);
}
#endif

[[maybe_unused]] static double time_ns(const config& c, bool retain, long n) {
    stores      s  = make_stores(c.cache_kind, c.mem_kind, retain, false);
    async_sp    sp = async_stack_create();
    str         key("k1");
    std::size_t sum  = call(c, s, key, sp);
    double      best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < n; ++i)
            sum += call(c, s, key, sp);
        auto t1 = std::chrono::steady_clock::now();
        best =
            std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
    }
    if (sum != std::size_t(7 * n + 1))
        std::abort();
    return best;
}

int main(int argc, char** argv) {
    if (argc >= 5 &&
        !std::strcmp(argv[1], "loop")) { // for callgrind: loop <config> <hit|miss> <n>
        const config& c      = configs[std::atoi(argv[2])];
        bool          retain = !std::strcmp(argv[3], "hit");
        long          n      = std::atol(argv[4]);
        stores        s      = make_stores(c.cache_kind, c.mem_kind, retain, false);
        async_sp      sp     = async_stack_create();
        str           key("k1");
        std::size_t   sum = call(c, s, key, sp);
        for (long i = 0; i < n; ++i)
            sum += call(c, s, key, sp);
        return sum == std::size_t(n + 1) ? 0 : 1;
    }
#ifdef COUNT_MOVES
    std::printf("%-36s| %-14s| key copies / moves | value copies / moves | operator new "
                "| slabs | async stack | caller's frame\n",
                "configuration",
                "path");
    for (const auto& c : configs) {
        counts(c, "hit", true, false);
        counts(c, "miss", false, false);
        if (c.root != 's')
            counts(c, "miss, deferred", false, true);
    }
#else
    long n = argc > 1 ? std::atol(argv[1]) : 2000000;
    for (const auto& c : configs)
        std::printf("%-36s hit %6.1f ns   miss %6.1f ns\n",
                    c.name,
                    time_ns(c, true, n),
                    time_ns(c, false, n));
#endif
}

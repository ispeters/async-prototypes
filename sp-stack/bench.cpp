// Measurements. This file sees only the interfaces.
#include "roots.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

template <class F>
static double best_ns(long per_rep, F&& f) {
    double best = 1e30;
    for (int rep = 0; rep < 7; ++rep) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        auto t1 = std::chrono::steady_clock::now();
        best    = std::min(
            best, std::chrono::duration<double, std::nano>(t1 - t0).count() / per_rep);
    }
    return best;
}

struct setup {
    stores            s;
    ms::mmap_provider roots{1 << 20, true};
    ms::stack         stk;
    async_sp          sp;
    str               key{"k1"};
    setup(bool retain, bool deferred)
        : s(make_stores(retain, deferred)),
          stk(roots.acquire()),
          sp(async_stack_create()) {
        // warm the cache, so that with `retain` every measured call is a hit
        call_sync(*s.sync, key);
        call_cursor(*s.cursor, key, sp);
        call_machine(*s.machine, key, stk);
    }
};

// column: 's' synchronous, 'c' cursor, 'm' machine stack
static std::size_t one_root_each(setup& u, char column, long n) {
    std::size_t sum = 0;
    for (long i = 0; i < n; ++i) {
        sum += column == 's'   ? call_sync(*u.s.sync, u.key)
               : column == 'c' ? call_cursor(*u.s.cursor, u.key, u.sp)
                               : call_machine(*u.s.machine, u.key, u.stk);
    }
    return sum;
}

static std::size_t one_root(setup& u, char column, long n) {
    return column == 's'   ? count_found_sync(*u.s.sync, u.key, n)
           : column == 'c' ? loop_cursor(*u.s.cursor, u.key, n, u.sp)
                           : loop_machine(*u.s.machine, u.key, n, u.stk);
}

static void times(long n) {
    struct path {
        const char* name;
        bool        retain, deferred;
    };
    static const path paths[] = {
        {"hit", true, false}, {"miss", false, false}, {"miss, suspending", false, true}};
    for (int nested = 0; nested < 2; ++nested) {
        std::printf("%s (ns per get)\n",
                    nested ? "n gets in a loop under one root"
                           : "each get under a root of its own");
        std::printf("  %-18s %12s %12s %12s\n", "", "synchronous", "cursor", "machine");
        for (const path& p : paths) {
            setup  u(p.retain, p.deferred);
            double t[3];
            int    col = 0;
            for (char c : {'s', 'c', 'm'}) {
                if (c == 's' && p.deferred) {
                    t[col++] = 0;
                    continue;
                }
                t[col++] = best_ns(n, [&] {
                    if ((nested ? one_root(u, c, n) : one_root_each(u, c, n)) !=
                        std::size_t(n)) {
                        std::abort();
                    }
                });
            }
            if (p.deferred) {
                std::printf("  %-18s %12s %12.1f %12.1f\n", p.name, "-", t[1], t[2]);
            } else {
                std::printf("  %-18s %12.1f %12.1f %12.1f\n", p.name, t[0], t[1], t[2]);
            }
        }
    }
}

static void forks(std::size_t kids, long reps) {
    std::vector<str>  keys(kids, "k1");
    std::size_t       chunk = 16 << 10;
    std::vector<char> buf(kids * chunk);
    std::printf("fork of %zu gets, joined (ns per child)\n", kids);
    std::printf("  %-18s %12s %12s %12s %12s\n",
                "",
                "cursor,",
                "machine,",
                "machine,",
                "machine,");
    std::printf("  %-18s %12s %12s %12s %12s\n",
                "children",
                "malloc slab",
                "pooled mmap",
                "fresh mmap",
                "user buffer");
    for (int deferred = 0; deferred < 2; ++deferred) {
        setup              u(false, deferred);
        ms::mmap_provider  pooled(64 << 10, true), fresh(64 << 10, false);
        ms::arena_provider arena(buf.data(), buf.size(), chunk);
        auto               run = [&](ms::stack_provider* p) {
            return best_ns(reps * kids, [&] {
                for (long r = 0; r < reps; ++r) {
                    std::size_t got =
                        p ? fork_machine(*u.s.machine, keys.data(), kids, *p, u.stk)
                          : fork_cursor(*u.s.cursor, keys.data(), kids, u.sp);
                    if (got != kids) {
                        std::abort();
                    }
                }
            });
        };
        double c = run(nullptr), a = run(&pooled), b = run(&fresh), d = run(&arena);
        std::printf("  %-18s %12.1f %12.1f %12.1f %12.1f\n",
                    deferred ? "suspend" : "complete inline",
                    c,
                    a,
                    b,
                    d);
    }
}

// How much stack each design uses for one get that suspends, and for a hit.
static void sizes() {
    std::printf("hand-written frame records (bytes): mem %zu, cache %zu, loop %zu "
                "(machine); mem %zu, cache %zu, loop %zu (cursor)\n",
                machine_frame_bytes[0],
                machine_frame_bytes[1],
                machine_frame_bytes[2],
                cursor_frame_bytes[0],
                cursor_frame_bytes[1],
                cursor_frame_bytes[2]);
    struct path {
        const char* name;
        bool        retain, deferred;
    };
    for (const path& p : {path{"hit", true, false},
                          path{"miss", false, false},
                          path{"miss, suspending", false, true}}) {
        setup u(p.retain, p.deferred);
        async_high_water = sizeof(async_slab);
        call_cursor(*u.s.cursor, u.key, u.sp);
        ms::fill(u.stk);
        ms::lowest_deferred_sp = nullptr;
        call_machine(*u.s.machine, u.key, u.stk);
        std::size_t peak = ms::high_water(u.stk);
        std::size_t live = ms::lowest_deferred_sp
                               ? static_cast<char*>(u.stk.top()) -
                                     static_cast<char*>(ms::lowest_deferred_sp)
                               : 0;
        std::printf("  %-18s cursor: %4zu B on the async stack | machine: %5zu B deepest",
                    p.name,
                    async_high_water - sizeof(async_slab),
                    peak);
        if (live) {
            std::printf(", %4zu B held while suspended", live);
        }
        std::printf("\n");
    }
}

int main(int argc, char** argv) {
    if (argc >= 6 &&
        !std::strcmp(
            argv[1],
            "loop")) { // for callgrind: loop <s|c|m> <each|nested> <hit|miss|suspend> <n>
        bool        nested = !std::strcmp(argv[3], "nested");
        setup       u(!std::strcmp(argv[4], "hit"), !std::strcmp(argv[4], "suspend"));
        long        n = std::atol(argv[5]);
        std::size_t got =
            nested ? one_root(u, argv[2][0], n) : one_root_each(u, argv[2][0], n);
        return got == std::size_t(n) ? 0 : 1;
    }
    if (argc >= 2 && !std::strcmp(argv[1], "sizes")) {
        return sizes(), 0;
    }
    long n = argc > 1 ? std::atol(argv[1]) : 2000000;
    times(n);
    forks(1000, std::max(1L, n / 20000));
}

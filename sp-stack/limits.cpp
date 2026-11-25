// What sizing a stack per strand runs into. Each subcommand is one experiment.
#include "roots.hpp"
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <vector>

static double now_ns() {
    return std::chrono::duration<double, std::nano>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static long rss_kib() {
    long pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        std::fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

// ---- maps: how many 64 KiB stacks with a guard page the OS hands out ----------
static int maps(std::size_t limit) {
    std::vector<ms::stack> v;
    long                   rss0 = rss_kib();
    double                 t0   = now_ns();
    while (v.size() < limit) {
        ms::stack s = ms::mmap_provider::map(64 << 10);
        if (!s.base)
            break;
        static_cast<char*>(s.top())[-1] = 1; // touch the first page a strand would use
        v.push_back(s);
    }
    double t1   = now_ns();
    long   rss1 = rss_kib();
    std::printf("mapped %zu stacks of 64 KiB + guard (%s); %.0f ns each, %.1f KiB "
                "resident each\n",
                v.size(),
                v.size() < limit ? "then mmap failed" : "the limit asked for",
                (t1 - t0) / v.size(),
                double(rss1 - rss0) / v.size());
    t0 = now_ns();
    for (ms::stack s : v)
        ms::mmap_provider::unmap(s);
    std::printf("unmapped at %.0f ns each\n", (now_ns() - t0) / v.size());
    return 0;
}

// ---- rss: memory held by n suspended children of one fork ---------------------
static int rss(char column, std::size_t n) {
    stores            s = make_stores(false, true);
    std::vector<str>  keys(n, "k1");
    slot<std::size_t> out;
    long              before = rss_kib();
    if (column == 'm') {
        ms::mmap_provider roots(8 << 20, true), kids(64 << 10, true);
        ms::stack         stk = roots.acquire();
        root_m            k(stk.top());
        ms::next          r = ms::enter(
            stk.top(), &k, ON_STRAND {
                return count_all_m(*s.machine, keys.data(), n, kids, &out, &k);
            });
        long during = rss_kib();
        k.drive(r);
        std::printf("machine: %zu suspended children hold %.2f KiB each (64 KiB stacks + "
                    "guard, one mapping pair each)\n",
                    n,
                    double(during - before) / n);
    } else {
        async_sp   sp = async_stack_create();
        root_c     k;
        async_next r      = count_all_c(*s.cursor, keys.data(), n, &out, &k, sp);
        long       during = rss_kib();
        k.drive(r);
        std::printf("cursor: %zu suspended children hold %.2f KiB each (one 4 KiB malloc "
                    "slab each)\n",
                    n,
                    double(during - before) / n);
    }
    return out.value == n ? 0 : 1;
}

// ---- overflow -----------------------------------------------------------------
[[gnu::noinline]] static int deep(int levels) noexcept { // about 1 KiB of stack per level
    volatile char buf[1000];
    for (std::size_t i = 0; i < sizeof(buf); ++i)
        buf[i] = 0x42;
    return levels ? deep(levels - 1) + buf[0] : buf[1];
}

static ms::stack g_guarded;
static void      on_segv(int, siginfo_t* si, void*) {
    char msg[160];
    long below = g_guarded.base - static_cast<char*>(si->si_addr);
    int  len   = std::snprintf(
        msg,
        sizeof(msg),
        "SIGSEGV %ld bytes below the stack's low end: the guard page caught it\n",
        below);
    if (write(1, msg, len) < 0)
        _exit(2);
    _exit(0);
}

static int overflow_guard() {
    static char alt[1 << 16];
    stack_t     ss{alt, 0, sizeof(alt)};
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = &on_segv;
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, nullptr);
    g_guarded = ms::mmap_provider::map(16 << 10);
    root_m k(g_guarded.top());
    std::printf(
        "a strand on a 16 KiB stack calls a synchronous function about 32 KiB deep\n");
    std::fflush(stdout);
    ms::enter(
        g_guarded.top(), &k, ON_STRAND {
            deep(32);
            return ms::next{&k, ASYNC_VALUE};
        });
    std::printf("no fault?\n");
    return 1;
}

static int overflow_arena() {
    static char        buf[2 * 16384];
    ms::arena_provider arena(buf, sizeof(buf), 16384);
    ms::stack          a = arena.acquire(), b = arena.acquire(); // b sits above a
    stores             s = make_stores(false, true);
    // strand A: one get, suspended
    str           arg("k1");
    slot<value_t> out;
    root_m        ka(a.top());
    ms::next      r = ms::enter(
        a.top(), &ka, ON_STRAND { return s.machine->get_entry(arg, &out, &ka); });
    std::size_t held =
        static_cast<char*>(a.top()) - static_cast<char*>(ms::lowest_deferred_sp);
    std::vector<char> saved(static_cast<char*>(a.top()) - held,
                            static_cast<char*>(a.top()));
    // strand B: the same deep synchronous call
    root_m kb(b.top());
    ms::enter(
        b.top(), &kb, ON_STRAND {
            deep(20);
            return ms::next{&kb, ASYNC_VALUE};
        });
    std::size_t changed = 0;
    for (std::size_t i = 0; i < held; ++i)
        changed += saved[i] != static_cast<char*>(a.top())[-long(held) + long(i)];
    std::printf("two strands on adjacent 16 KiB pieces of a user's buffer; A is "
                "suspended holding %zu B\n"
                "B's synchronous callee ran about 20 KiB deep and returned normally\n"
                "%zu of A's %zu suspended bytes were overwritten; nothing reported it\n",
                held,
                changed,
                held);
    (void)r; // A is never resumed: its frames are gone
    return changed ? 0 : 1;
}

// ---- depths: how deep ordinary synchronous callees go -------------------------
template <class F>
static void depth_of(const char* what, ms::stack stk, F&& f) {
    f(); // once on the thread's stack first, so lazy binding and first-use setup aren't
         // counted
    ms::fill(stk);
    root_m k(stk.top());
    ms::enter(stk.top(), &k, [&] [[gnu::always_inline]] () noexcept {
        f();
        return ms::next{&k, ASYNC_VALUE};
    });
    std::printf("  %-52s %7zu B\n", what, ms::high_water(stk));
}

static int depths() {
    ms::stack stk = ms::mmap_provider::map(8 << 20);
    stores    s   = make_stores(false, false);
    std::printf("deepest stack use of one synchronous call made from a strand:\n");
    static volatile double d = 3.141592653589793e100;
    static char            out[512];
    depth_of(
        "cache miss, kv_store::get_sync", stk, [&] { (void)s.sync->get_sync("k1"); });
    depth_of("snprintf(\"%d\")", stk, [&] {
        std::snprintf(out, sizeof(out), "%d", int(d > 0));
    });
    depth_of("snprintf(\"%f\") of 3.14e100", stk, [&] {
        std::snprintf(out, sizeof(out), "%f", double(d));
    });
    depth_of("std::ostringstream << double", stk, [&] {
        std::ostringstream os;
        os << double(d);
    });
    depth_of("std::sort of 10000 strings", stk, [&] {
        std::vector<std::string> v;
        for (int i = 0; i < 10000; ++i)
            v.push_back(std::to_string(i * 7919 % 10007));
        std::sort(v.begin(), v.end());
    });
    depth_of("std::regex_match, (a|b)*c on 2000 characters", stk, [&] {
        std::string in(2000, 'a');
        in += 'c';
        (void)std::regex_match(in, std::regex("(a|b)*c"));
    });
    depth_of("fopen + fgets + fclose of /proc/self/stat", stk, [&] {
        if (FILE* f = std::fopen("/proc/self/stat", "r")) {
            (void)!std::fgets(out, sizeof(out), f);
            std::fclose(f);
        }
    });
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "maps"))
        return maps(argc > 2 ? std::atol(argv[2]) : 100000);
    if (argc >= 4 && !std::strcmp(argv[1], "rss"))
        return rss(argv[2][0], std::atol(argv[3]));
    if (argc >= 2 && !std::strcmp(argv[1], "overflow-guard"))
        return overflow_guard();
    if (argc >= 2 && !std::strcmp(argv[1], "overflow-arena"))
        return overflow_arena();
    if (argc >= 2 && !std::strcmp(argv[1], "depths"))
        return depths();
    std::printf("usage: limits maps [n] | rss <m|c> <n> | overflow-guard | "
                "overflow-arena | depths\n");
    return 2;
}

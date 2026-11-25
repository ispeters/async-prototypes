// The call tree: correctness with suspending leaves, then cost per call.
#include "tree.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<node> nodes;
static const node*       build(int depth) { // a full tree; nodes were reserved up front
    if (depth == 0) {
        return nullptr;
    }
    std::size_t i = nodes.size();
    nodes.push_back({nullptr, nullptr, long(i * 2654435761u % 1000)});
    const node* l  = build(depth - 1);
    const node* r  = build(depth - 1);
    nodes[i].left  = l;
    nodes[i].right = r;
    return &nodes[i];
}

struct root_c : async_cont {
    int how = ASYNC_PENDING;
    root_c() {
        resume = [](async_cont* c, int h) noexcept -> async_next {
            static_cast<root_c*>(c)->how = h;
            return {nullptr, ASYNC_PENDING};
        };
    }
};
struct root_m : ms::cont {
    int how = ASYNC_PENDING;
    explicit root_m(void* top) {
        resume = [](ms::cont* c, int h) noexcept -> ms::next {
            static_cast<root_m*>(c)->how = h;
            return {nullptr, ASYNC_PENDING};
        };
        sp = top;
    }
};

[[gnu::noinline]] static long run_c(const node* t, async_sp sp) {
    long       out;
    root_c     k;
    async_next n = walk_c(t, &out, &k, sp);
    if (n.next == &k) {
        k.how = n.how;
    } else {
        async_run(n);
    }
    while (k.how == ASYNC_PENDING) {
        async_run_pending();
    }
    return out;
}

[[gnu::noinline]] static long run_m(const node* t, ms::stack stk) {
    long     out;
    root_m   k(stk.top());
    ms::next n = ms::enter(stk.top(), &k, [&] [[gnu::always_inline]] () noexcept {
        return walk_m(t, &out, &k);
    });
    if (n.k == &k) {
        k.how = n.how;
    } else {
        ms::run(n);
    }
    while (k.how == ASYNC_PENDING) {
        ms::run_pending();
    }
    return out;
}

int main(int argc, char** argv) {
    int depth = 16;
    nodes.reserve((std::size_t(1) << depth) - 1);
    const node*       t     = build(depth);
    long              calls = (long(1) << (depth + 1)) - 1;
    ms::mmap_provider roots(1 << 20, true);
    ms::stack         stk  = roots.acquire();
    async_sp          sp   = async_stack_create();
    long              want = walk_sync(t);

    if (argc >= 4 &&
        !std::strcmp(argv[1], "loop")) { // for callgrind: loop <s|c|m> <walks>
        long sum = 0, reps = std::atol(argv[3]);
        for (long i = 0; i < reps; ++i) {
            sum += argv[2][0] == 's'   ? walk_sync(t)
                   : argv[2][0] == 'c' ? run_c(t, sp)
                                       : run_m(t, stk);
        }
        return sum == reps * want ? 0 : 1;
    }

    // "inline" skips everything that suspends, for builds with MS_NO_CLOBBER
    int paths = (argc >= 2 && !std::strcmp(argv[1], "inline")) ? 1 : 2;

    // correctness, completing inline and with every leaf suspending
    for (int suspend = 0; suspend < paths; ++suspend) {
        walk_leaves_suspend    = suspend;
        async_high_water       = sizeof(async_slab);
        ms::lowest_deferred_sp = nullptr;
        ms::fill(stk);
        if (run_c(t, sp) != want || run_m(t, stk) != want) {
            return std::printf("FAILED\n"), 1;
        }
        if (suspend) {
            std::size_t live = static_cast<char*>(stk.top()) -
                               static_cast<char*>(ms::lowest_deferred_sp);
            std::printf("suspended %d calls deep: cursor %zu B on the async stack (%zu B "
                        "per call); "
                        "machine %zu B held (%zu B per call), %zu B deepest\n",
                        depth + 1,
                        async_high_water - sizeof(async_slab),
                        (async_high_water - sizeof(async_slab)) / (depth + 1),
                        live,
                        live / (depth + 1),
                        ms::high_water(stk));
        } else {
            std::printf("completing inline: machine %zu B deepest (%zu B per call)\n",
                        ms::high_water(stk),
                        ms::high_water(stk) / (depth + 1));
        }
    }
    std::printf("hand-written frame records: cursor %zu B, machine %zu B\n",
                walk_frame_bytes[0],
                walk_frame_bytes[1]);

    for (int suspend = 0; suspend < paths; ++suspend) {
        walk_leaves_suspend = suspend;
        double best[3]      = {1e30, 1e30, 1e30};
        for (int rep = 0; rep < 15; ++rep) {
            for (int col = suspend; col < 3; ++col) {
                auto t0  = std::chrono::steady_clock::now();
                long got = col == 0   ? walk_sync(t)
                           : col == 1 ? run_c(t, sp)
                                      : run_m(t, stk);
                auto t1  = std::chrono::steady_clock::now();
                if (got != want) {
                    std::abort();
                }
                best[col] = std::min(
                    best[col],
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / calls);
            }
        }
        if (suspend) {
            std::printf("every leaf suspends, ns per call:  cursor %.2f  machine %.2f\n",
                        best[1],
                        best[2]);
        } else {
            std::printf("completing inline, ns per call:    synchronous %.2f  cursor "
                        "%.2f  machine %.2f\n",
                        best[0],
                        best[1],
                        best[2]);
        }
    }
}

// The measurements. Variant -1 is the synchronous yardstick.
//   bench                        timings (ns per call)
//   bench loop <v> <w> <n>       for callgrind
//   (built with -DCOUNT_MOVES -DASYNC_STATS) copies, moves and frame storage
#include "callers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool applies(int v, int w) {
  // the synchronous error path throws; or_else has no synchronous column
  return v >= 0 || w == w_get_hit || w == w_get_miss || w == w_get_or_value || w == w_pass2_value;
}

#ifdef COUNT_MOVES
template <class... Vs>
static void records(tlist<Vs...>) {
  ((std::printf("%-22s record %2zu B; root<str> %2zu B\n", Vs::name, sizeof(typename Vs::rec),
                sizeof(root<Vs, str>))),
   ...);
}

static void counts(int v, int w, bool deferred) {
  rig_base* r = make_rig(v, w, deferred);
  if (w == w_get_hit) r->call();  // warm the cache, so the measured call is a hit
  str::key_copies = str::key_moves = str::val_copies = str::val_moves = 0;
  async_high_water = sizeof(async_slab);
  std::size_t got = r->call();
  if (got != expected(w)) std::abort();
  std::printf("%-22s| %-16s| %-8s| %ld / %ld | %ld / %ld | %zu B\n", variant_name(v), workload_names[w],
              deferred ? "deferred" : "inline", str::key_copies, str::key_moves, str::val_copies, str::val_moves,
              async_high_water - sizeof(async_slab));
  delete r;
}
#endif

[[maybe_unused]] static double time_ns(int v, int w, long n) {
  rig_base* r = make_rig(v, w, false);
  std::size_t sum = r->call();
  double best = 1e30;
  for (int rep = 0; rep < 7; ++rep) {
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) sum += r->call();
    auto t1 = std::chrono::steady_clock::now();
    best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / n);
  }
  if (sum != expected(w) * std::size_t(7 * n + 1)) std::abort();
  delete r;
  return best;
}

int main(int argc, char** argv) {
  if (argc >= 5 && !std::strcmp(argv[1], "loop")) {
    int v = std::atoi(argv[2]), w = std::atoi(argv[3]);
    long n = std::atol(argv[4]);
    rig_base* r = make_rig(v, w, false);
    std::size_t sum = r->call();
    for (long i = 0; i < n; ++i) sum += r->call();
    return sum == expected(w) * std::size_t(n + 1) ? 0 : 1;
  }
#ifdef COUNT_MOVES
  records(all_variants{});
  std::printf("%-22s| %-16s| %-8s| key copies / moves | value copies / moves | async stack\n", "variant", "workload",
              "backing");
  for (int w = 0; w < n_workloads; ++w)
    for (int v = -1; v < n_variants; ++v)
      if (applies(v, w))
        for (bool d : {false, true})
          if (v >= 0 || !d) counts(v, w, d);
#else
  long n = argc > 1 ? std::atol(argv[1]) : 2000000;
  for (int w = 0; w < n_workloads; ++w)
    for (int v = -1; v < n_variants; ++v)
      if (applies(v, w)) std::printf("%-22s %-16s %7.1f ns\n", variant_name(v), workload_names[w], time_ns(v, w, n));
#endif
}

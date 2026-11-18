// Every path through both designs, against the synchronous answer.
#include "roots.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

static int failures;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);    \
      ++failures;                                                   \
    }                                                               \
  } while (0)

// Writes over 2 KiB of stack below the caller.
static int scribbles;
[[gnu::noinline]] static void scribble() noexcept {
  ++scribbles;
  volatile char buf[2048];
  for (std::size_t i = 0; i < sizeof(buf); ++i) buf[i] = 0x5A;
}

int main() {
  ms::mmap_provider roots(1 << 20, true);
  ms::stack stk = roots.acquire();
  async_sp sp = async_stack_create();

  // one get: hit, miss, absent; inline and deferred
  for (int deferred = 0; deferred < 2; ++deferred)
    for (int retain = 0; retain < 2; ++retain) {
      stores s = make_stores(retain, deferred);
      for (char const* key : {"k1", "nope", "k1", "k2", "k1"}) {
        std::size_t want = call_sync(*s.sync, key);
        CHECK(call_cursor(*s.cursor, key, sp) == want);
        CHECK(call_machine(*s.machine, key, stk) == want);
      }
    }

  // a loop of awaits, and that the stack doesn't creep while it runs
  for (int deferred = 0; deferred < 2; ++deferred)
    for (int retain = 0; retain < 2; ++retain) {
      stores s = make_stores(retain, deferred);
      CHECK(count_found_sync(*s.sync, "k1", 1000) == 1000);
      CHECK(loop_cursor(*s.cursor, "k1", 1000, sp) == 1000);
      ms::fill(stk);
      CHECK(loop_machine(*s.machine, "k1", 10, stk) == 10);
      std::size_t after_10 = ms::high_water(stk);
      CHECK(loop_machine(*s.machine, "k1", 1000, stk) == 1000);
      CHECK(ms::high_water(stk) == after_10);
      CHECK(loop_machine(*s.machine, "nope", 1000, stk) == 0);
    }

  // a caller that carries on after its callee suspended: the callee's frame
  // has to survive what the caller does to the stack
  {
    stores s = make_stores(false, true);
    ms::after_pending = &scribble;
    CHECK(call_machine(*s.machine, "k1", stk) == 1);
    CHECK(call_machine(*s.machine, "nope", stk) == 0);
    CHECK(loop_machine(*s.machine, "k1", 100, stk) == 100);
    ms::after_pending = nullptr;
    CHECK(scribbles == 102);
    // and the suspended frames really were on the strand's stack
    CHECK(ms::lowest_deferred_sp > stk.base && ms::lowest_deferred_sp < stk.top());
  }

  // forks: every provider, children inline and deferred
  {
    std::vector<str> keys;
    for (int i = 0; i < 200; ++i) keys.push_back(i % 3 == 0 ? "nope" : i % 3 == 1 ? "k1" : "k2");
    std::size_t want = 133;
    static char buf[200 * 16384];
    ms::mmap_provider pooled(64 << 10, true), fresh(64 << 10, false);
    ms::arena_provider arena(buf, sizeof(buf), 16384);
    for (int deferred = 0; deferred < 2; ++deferred)
      for (int retain = 0; retain < 2; ++retain) {
        stores s = make_stores(retain, deferred);
        CHECK(fork_cursor(*s.cursor, keys.data(), keys.size(), sp) == want);
        for (ms::stack_provider* p : {static_cast<ms::stack_provider*>(&pooled),
                                      static_cast<ms::stack_provider*>(&fresh),
                                      static_cast<ms::stack_provider*>(&arena)})
          for (int twice = 0; twice < 2; ++twice)
            CHECK(fork_machine(*s.machine, keys.data(), keys.size(), *p, stk) == want);
      }
    CHECK(pooled.mapped == 200);  // the second and later forks reuse the first's stacks
    // with a scribbling parent in every child
    stores s = make_stores(false, true);
    ms::after_pending = &scribble;
    CHECK(fork_machine(*s.machine, keys.data(), keys.size(), pooled, stk) == want);
    ms::after_pending = nullptr;
  }

  std::printf(failures ? "%d FAILED\n" : "all checks passed\n", failures);
  return failures != 0;
}

// Scenarios to count instructions in, one mode per program.
//
//   bench SCENARIO N
//
// runs SCENARIO N times (for `repeat`, once, with N iterations). cg.sh runs
// each twice under Callgrind and reports the difference per run.
#include "algos.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

using namespace th;

volatile int sink;

[[gnu::noinline]] void chain4(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(then(then(then(just(x), inc), inc), inc), inc),
           [](auto& st) { sink = *st.value; });
}

[[gnu::noinline]] void chain4_throwing(int x) {
  auto inc = [](int v) {
    if (v < 0) throw std::runtime_error("negative");
    return v + 1;
  };
  run_root(then(then(then(then(just(x), inc), inc), inc), inc),
           [](auto& st) { sink = *st.value; });
}

[[gnu::noinline]] void let(int x) {
  run_root(let_value(just(x), [](int& v) noexcept { return just(v + 1); }),
           [](auto& st) { sink = *st.value; });
}

[[gnu::noinline]] void both(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(when_all(then(just(x), inc), then(just(x), inc)),
           [](auto& st) { sink = (*st.value).first + (*st.value).second; });
}

[[gnu::noinline]] void boundary(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(erase_boundary(then(just(x), inc)), inc),
           [](auto& st) { sink = *st.value; });
}

[[gnu::noinline]] void repeat(int n) {
  int count = 0;
  run_root(repeat_until(then(just(&count),
                             [n](int* c) noexcept { return ++*c >= n; })),
           [](auto&) {});
  sink = count;
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s SCENARIO N\n", argv[0]);
    return 2;
  }
  int n = std::atoi(argv[2]);
  char const* s = argv[1];
  if (!std::strcmp(s, "repeat")) {
    repeat(n);
    return sink == n ? 0 : 1;
  }
  void (*f)(int) = !std::strcmp(s, "chain4")            ? chain4
                   : !std::strcmp(s, "chain4_throwing") ? chain4_throwing
                   : !std::strcmp(s, "let")             ? let
                   : !std::strcmp(s, "both")            ? both
                   : !std::strcmp(s, "boundary")        ? boundary
                                                        : nullptr;
  if (!f) {
    std::fprintf(stderr, "unknown scenario %s\n", s);
    return 2;
  }
  for (int i = 0; i < n; ++i) {
    f(i);
  }
  return 0;
}

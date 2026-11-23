// Scenarios to count instructions in, one mode per program.
//
//   bench SCENARIO N
//
// runs SCENARIO N times (for `repeat` and `repeat_checked`, once, with N
// iterations). cg.sh runs each twice under Callgrind and reports the
// difference per run. The `_checked` scenarios differ from their namesakes
// only in a leaf that asks a stop token, which the root provides.
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
           [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

[[gnu::noinline]] void chain4_throwing(int x) {
  auto inc = [](int v) {
    if (v < 0) throw std::runtime_error("negative");
    return v + 1;
  };
  run_root(then(then(then(then(just(x), inc), inc), inc), inc),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

[[gnu::noinline]] void let(int x) {
  run_root(let_value(just(x), [](int& v) noexcept { return just(v + 1); }),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

[[gnu::noinline]] void both(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(when_all(then(just(x), inc), then(just(x), inc)),
           [](auto& st) { auto& v = st.template get<set_value_t(int, int)>();
             sink = v.template get<0>() + v.template get<1>(); });
}

[[gnu::noinline]] void boundary(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(erase_boundary(then(just(x), inc)), inc),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

// The same, with a leaf that asks the stop token. `live` is never stopped.
inplace_stop_source live;

[[gnu::noinline]] void chain4_checked(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(then(then(then(just_unless_stopped(x), inc), inc), inc), inc),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); }, live.get_token());
}

// The leaf asks, but the root has no stop token.
[[gnu::noinline]] void chain4_unstoppable(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(then(then(then(just_unless_stopped(x), inc), inc), inc), inc),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); });
}

// when_all under a stoppable parent, with leaves that don't ask: only the
// forwarding callback's registration differs from `both`.
[[gnu::noinline]] void both_live(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(when_all(then(just(x), inc), then(just(x), inc)),
           [](auto& st) { auto& v = st.template get<set_value_t(int, int)>();
             sink = v.template get<0>() + v.template get<1>(); },
           live.get_token());
}

[[gnu::noinline]] void both_checked(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(when_all(then(just_unless_stopped(x), inc),
                    then(just_unless_stopped(x), inc)),
           [](auto& st) { auto& v = st.template get<set_value_t(int, int)>();
             sink = v.template get<0>() + v.template get<1>(); },
           live.get_token());
}

[[gnu::noinline]] void boundary_checked(int x) {
  auto inc = [](int v) noexcept { return v + 1; };
  run_root(then(erase_boundary(then(just_unless_stopped(x), inc)), inc),
           [](auto& st) { sink = st.template get<set_value_t(int)>(); }, live.get_token());
}

[[gnu::noinline]] void repeat_checked(int n) {
  int count = 0;
  run_root(repeat_until(then(just_unless_stopped(&count),
                             [n](int* c) noexcept { return ++*c >= n; })),
           [](auto&) {}, live.get_token());
  sink = count;
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
  if (!std::strcmp(s, "repeat") || !std::strcmp(s, "repeat_checked")) {
    (!std::strcmp(s, "repeat") ? repeat : repeat_checked)(n);
    return sink == n ? 0 : 1;
  }
  void (*f)(int) = !std::strcmp(s, "chain4")            ? chain4
                   : !std::strcmp(s, "chain4_throwing") ? chain4_throwing
                   : !std::strcmp(s, "let")             ? let
                   : !std::strcmp(s, "both")            ? both
                   : !std::strcmp(s, "boundary")        ? boundary
                   : !std::strcmp(s, "chain4_checked")  ? chain4_checked
                   : !std::strcmp(s, "chain4_unstoppable") ? chain4_unstoppable
                   : !std::strcmp(s, "both_live")       ? both_live
                   : !std::strcmp(s, "both_checked")    ? both_checked
                   : !std::strcmp(s, "boundary_checked") ? boundary_checked
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

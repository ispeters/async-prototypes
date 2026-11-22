// Checks for every mode: results, errors, an immovable value, a loop's stack
// depth, an erased boundary, and (TYPED mode) which handle types come back.
#include "algos.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#define CHECK(x)                                                    \
  do {                                                              \
    if (!(x)) {                                                     \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x);       \
      std::exit(1);                                                 \
    }                                                               \
  } while (0)

using namespace th;

struct pinned {
  int v;
  explicit pinned(int x) : v(x) {}
  pinned(pinned&&) = delete;
};

// The extent of the machine stack seen from inside a loop's body.
char* stack_lo = nullptr;
char* stack_hi = nullptr;
[[gnu::noinline]] void probe() {
  char* p = static_cast<char*>(__builtin_frame_address(0));
  if (!stack_lo || p < stack_lo) stack_lo = p;
  if (!stack_hi || p > stack_hi) stack_hi = p;
}

const char* mode_name() {
  switch (TH_MODE) {
    case TH_EAGER: return "EAGER";
    case TH_ERASED: return "ERASED";
    case TH_TYPED: return "TYPED";
    default: return "VARIANT";
  }
}

int main() {
  auto inc = [](int x) noexcept { return x + 1; };

  {  // then
    run_root(then(just(1), inc), [](auto& st) {
      CHECK(st.value.has_value() && *st.value == 2);
    });
  }
  {  // a chain of four
    run_root(then(then(then(then(just(1), inc), inc), inc), inc),
             [](auto& st) { CHECK(*st.value == 5); });
  }
  {  // a function that may throw, and does
    auto boom = [](int x) -> int {
      if (x > 0) throw std::runtime_error("boom");
      return x;
    };
    run_root(then(just(1), boom), [](auto& st) {
      CHECK(!st.value.has_value() && st.error.has_value());
    });
    run_root(then(just(0), boom), [](auto& st) {
      CHECK(st.value.has_value() && *st.value == 0);
    });
  }
  {  // an immovable value, constructed where the root keeps it
    run_root(then(just(7), [](int x) noexcept { return pinned(x); }),
             [](auto& st) { CHECK(st.value.has_value() && (*st.value).v == 7); });
  }
  {  // let_value, the second child borrowing the first's value
    run_root(let_value(just(std::string("ab")),
                       [](std::string& s) noexcept { return just(s + "c"); }),
             [](auto& st) { CHECK(*st.value == "abc"); });
  }
  {  // when_all
    run_root(when_all(then(just(1), inc), just(std::string("x"))),
             [](auto& st) {
               CHECK(st.value.has_value());
               CHECK((*st.value).first == 2 && (*st.value).second == "x");
             });
    auto boom = [](int) -> int { throw 1; };
    run_root(when_all(then(just(1), boom), just(2)), [](auto& st) {
      CHECK(!st.value.has_value() && st.error.has_value());
    });
  }
  {  // an erased boundary in the middle of a chain
    run_root(then(erase_boundary(then(just(1), inc)), inc),
             [](auto& st) { CHECK(*st.value == 3); });
  }
  // Stop tokens.
  {
    inplace_stop_source live;
    run_root(then(just_unless_stopped(1), inc),
             [](auto& st) { CHECK(*st.value == 2); }, live.get_token());
    inplace_stop_source stopped;
    stopped.request_stop();
    run_root(then(just_unless_stopped(1), inc),
             [](auto& st) { CHECK(st.stopped && !st.value.has_value()); },
             stopped.get_token());
    // no token at all: just_unless_stopped is just
    run_root(then(just_unless_stopped(1), inc),
             [](auto& st) { CHECK(*st.value == 2); });
  }
  {  // a child's failure stops its sibling, through when_all's own source
    bool ran = false;
    auto boom = [](int) -> int { throw 1; };
    run_root(when_all(then(just(1), boom),
                      then(just_unless_stopped(2),
                           [&ran](int x) noexcept {
                             ran = true;
                             return x;
                           })),
             [](auto& st) { CHECK(st.error.has_value()); });
    CHECK(!ran);
  }
  {  // the parent's stop reaches when_all's children, through a callback
    inplace_stop_source stopped;
    stopped.request_stop();
    run_root(when_all(just_unless_stopped(1), just_unless_stopped(2)),
             [](auto& st) { CHECK(st.stopped); }, stopped.get_token());
    inplace_stop_source live;  // registered, then deregistered
    run_root(when_all(just_unless_stopped(1), just_unless_stopped(2)),
             [](auto& st) { CHECK((*st.value).first == 1); }, live.get_token());
    CHECK(!live.stop_requested());
  }
  {  // through an erased boundary, by its one forwarded query
    inplace_stop_source stopped;
    stopped.request_stop();
    run_root(then(erase_boundary(then(just_unless_stopped(1), inc)), inc),
             [](auto& st) { CHECK(st.stopped); }, stopped.get_token());
    inplace_stop_source live;
    run_root(then(erase_boundary(then(just_unless_stopped(1), inc)), inc),
             [](auto& st) { CHECK(*st.value == 3); }, live.get_token());
  }
  {  // a loop stopped from inside its body
    inplace_stop_source src;
    int count = 0;
    run_root(repeat_until(then(just_unless_stopped(&count),
                               [&src](int* c) noexcept {
                                 if (++*c == 5) src.request_stop();
                                 return false;
                               })),
             [](auto& st) { CHECK(st.stopped); }, src.get_token());
    CHECK(count == 5);
  }

  // repeat_until: count to N, measuring the stack from inside the body.
  long extent[2] = {};
  for (int i = 0; int n : {10, 1000}) {
    int count = 0;
    stack_lo = stack_hi = nullptr;
    run_root(repeat_until(then(just(&count),
                               [n](int* c) noexcept {
                                 probe();
                                 return ++*c >= n;
                               })),
             [](auto& st) { CHECK(st.value.has_value()); });
    CHECK(count == n);
    extent[i++] = long(stack_hi - stack_lo);
    std::printf("%s repeat_until %4d iterations: stack extent %ld bytes\n",
                mode_name(), n, extent[i - 1]);
  }
#if TH_MODE != TH_EAGER
  CHECK(extent[0] == extent[1]);  // bounded: the same whatever n is
#endif

  // Which handles come back. In TYPED mode a then whose function can't throw
  // hands its parent a typed handle; one that can throw has two outcomes, so
  // its completion is erased, or in VARIANT mode a choice of the two.
  {
    using R = root_rcvr<int>;
    using nothrow_op = then_op<just_sender<int>, decltype(inc), R>;
    using start_t_ = decltype(std::declval<nothrow_op&>().start());
    using complete_t_ = decltype(std::declval<nothrow_op&>().invoke());
#if TH_MODE == TH_TYPED || TH_MODE == TH_VARIANT
    static_assert(std::same_as<start_t_,
                               operation_handle<nothrow_op::rcvr_t, set_value_t(int)>>);
    static_assert(std::same_as<complete_t_, operation_handle<R, set_value_t(int)>>);
    auto may_throw = [](int x) { return x; };
    using throwing_op = then_op<just_sender<int>, decltype(may_throw), R>;
    using throwing_t = decltype(std::declval<throwing_op&>().invoke());
#if TH_MODE == TH_TYPED
    static_assert(std::same_as<throwing_t, erased_handle>);
#else  // a choice of the two completions
    static_assert(std::same_as<
                  throwing_t, choice<operation_handle<R, set_value_t(int)>,
                                     operation_handle<R, set_error_t(exception_ptr)>>>);
#endif
#elif TH_MODE == TH_ERASED
    static_assert(std::same_as<start_t_, erased_handle>);
    static_assert(std::same_as<complete_t_, erased_handle>);
#else
    static_assert(std::same_as<start_t_, no_operation>);
    static_assert(std::same_as<complete_t_, no_operation>);
#endif
    static_assert(sizeof(erased_handle) == 2 * sizeof(void*));
  }

  std::printf("%s checks pass\n", mode_name());
}

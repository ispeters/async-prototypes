#include "algos26.hpp"
#include "counted.hpp"
#include <cstdio>
#include <cstdlib>
#include <thread>
namespace k = c26;
using c26::get_scheduler; using c26::prop; using c26::receiver_t; using c26::exception_ptr; namespace execution = STDEXEC;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

struct tag_q { int query(tag_q) const noexcept { return 0; } };

int main() {
  // k::then over just
  {
    auto s = k::then(k::just(counted("hello")), [](counted&& c) { return int(c.s.size()); });
    moves = 0;
    auto r = c26::sync_wait(std::move(s));
    CHECK(r && *r == 5);
    std::printf("C++26 k::then(just): moves=%d\n", moves);
  }
  {
    auto s = k::just(counted("hello"));
    moves = 0;
    auto r = c26::sync_wait(std::move(s));
    CHECK(r && r->s == "hello");
    std::printf("C++26 just: moves=%d\n", moves);
  }
  // write_env
  {
    auto r = c26::sync_wait(k::write_env(k::just(7), prop{get_scheduler, 0}));
    CHECK(r && *r == 7);
  }
  // let_value
  {
    auto s = k::let_value(k::just(counted("ab")), [](counted& c) { return k::just(counted(c.s + "c")); });
    moves = 0;
    auto r = c26::sync_wait(std::move(s));
    CHECK(r && r->s == "abc");
    std::printf("C++26 let_value: moves=%d\n", moves);
  }
  // when_all: value and error
  {
    auto s = k::then(k::when_all(k::just(counted("x")), k::just(counted("yy"))),
                  [](counted&& a, counted&& b) { return int(a.s.size() + b.s.size()); });
    moves = 0;
    auto r = c26::sync_wait(std::move(s));
    CHECK(r && *r == 3);
    std::printf("C++26 k::then(when_all): moves=%d\n", moves);
    bool threw = false;
    try {
      c26::sync_wait(k::then(k::when_all(k::just(1), k::just_error_sender<int>{}), [](int, int) { return 0; }));
    } catch (int i) {
      threw = (i == 42);
    }
    CHECK(threw);
  }
  // k::run_loop from another thread
  {
    k::run_loop loop;
    std::optional<int> got;
    struct rcvr_t {
      using receiver_concept = receiver_t;
      k::run_loop* loop;
      std::optional<int>* got;
      void set_value(int i) && noexcept { *got = i; loop->finish(); }
      void set_error(exception_ptr) && noexcept { loop->finish(); }
      void set_stopped() && noexcept { loop->finish(); }
    };
    auto op = execution::connect(k::then(loop.get_scheduler().schedule(), [] { return 9; }), rcvr_t{&loop, &got});
    std::thread t([&] { execution::start(op); });
    loop.run();
    t.join();
    CHECK(got && *got == 9);
  }
#define COL "C++26"
#define SUM [](counted&& a, counted&& b) { return int(a.s.size() + b.s.size()); }
#include "fresh_body.inc"
  std::printf("C++26 checks pass\n");
}

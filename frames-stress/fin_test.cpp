// Sandbox-only test of the phase-based finally: the receiver destroys the
// operation from inside its completion, under every policy and layout, with
// values whose copies may throw at every point in turn. Build with
// -DPART=0..3.
#include "finally.hpp"
#include "just.hpp"
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace ex = stdexec;
namespace ts = frames::typestate;

// A string whose countdown-th copy or move throws.
inline int countdown = -1;
struct fragile {
  std::string s;
  explicit fragile(std::string v) : s(std::move(v)) {}
  fragile(fragile const& o) : s(o.s) { tick(); }
  fragile(fragile&& o) : s(std::move(o.s)) { tick(); }
  static void tick() { if (countdown > 0 && --countdown == 0) throw std::runtime_error("fragile"); }
};

struct ctx {
  std::atomic<bool> finished{false};
  void* op = nullptr;
  void (*del)(void*) = nullptr;
  std::string got;
  void done(std::string what) noexcept {
    got = std::move(what);  // copied before the operation is destroyed
    del(op);
    finished.store(true, std::memory_order_release);
    finished.notify_one();
  }
};

template <class P> struct rcvr {
  using receiver_concept = ex::receiver_t;
  ctx* c;
  auto get_env() const noexcept -> ts::policies_env<P> { return {}; }
  template <class... A> void set_value(A&&... a) && noexcept {
    std::string s = "value";
    ((s += " " + std::string(a.s)), ...);
    c->done(std::move(s));
  }
  template <class E> void set_error(E&& e) && noexcept {
    if constexpr (std::is_same_v<std::decay_t<E>, fragile>) c->done("error " + e.s);
    else if constexpr (std::is_same_v<std::decay_t<E>, int>) c->done("error " + std::to_string(e));
    else c->done("exception");
  }
  void set_stopped() && noexcept { c->done("stopped"); }
};

template <class P, class Sndr> std::string run(Sndr&& sndr) {
  ctx c;
  using op_t = ex::connect_result_t<Sndr, rcvr<P>>;
  op_t* op = nullptr;
  try { op = new op_t(ex::connect(std::move(sndr), rcvr<P>{&c})); }
  catch (...) { return "connect threw"; }
  c.op = op;
  c.del = [](void* p) { delete static_cast<op_t*>(p); };
  ex::start(*op);
  c.finished.wait(false, std::memory_order_acquire);
  return c.got;
}

exec::static_thread_pool pool(2);

// Runs make() with the n-th copy or move throwing, for n = 1, 2, ... until
// none does; returns how many runs that took.
// After a throw: "exception", or what final completed with if it failed
// (a throw inside initial becomes initial's error, and final still runs).
template <class P, class Make> int every_failure(Make make, std::string const& expect, std::string const& final_outcome = "exception") {
  int n = 1;
  for (;; ++n) {
    auto sndr = make();
    countdown = n;
    std::string got = run<P>(std::move(sndr));
    bool threw = countdown == 0;
    countdown = -1;
    if (!threw) {
      if (got != expect) { std::fprintf(stderr, "    n=%d expected %s, got %s\n", n, expect.c_str(), got.c_str()); std::abort(); }
      return n;
    }
    if (got != "exception" && got != "connect threw" && got != final_outcome) { std::fprintf(stderr, "    n=%d after a throw: %s\n", n, got.c_str()); std::abort(); }
  }
}

template <class P> int all(char const* name) {
  auto s = pool.get_scheduler();
  int runs = 0;
  auto final_ok = [] { return ts::just(); };
  runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a"), fragile("b")), final_ok()); }, "value a b");
  runs += every_failure<P>([&] { return ts::finally(ex::just_error(fragile("e")), final_ok()); }, "error e");
  runs += every_failure<P>([&] { return ts::finally(ex::just_stopped(), final_ok()); }, "stopped");
  runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a")), ex::just_error(7)); }, "error 7", "error 7");
  runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a")), ex::just_stopped()); }, "stopped", "stopped");
  // final's own error, copied out of the slot before it ends.
  runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a")), ex::just_error(fragile("f"))); }, "error f", "error f");
  for (int i = 0; i < 200; ++i) {
    runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a")), ex::starts_on(s, ex::just())); }, "value a");
    runs += every_failure<P>([&] { return ts::finally(ex::starts_on(s, ex::just(fragile("b"))), ts::just()); }, "value b");
    runs += every_failure<P>([&] { return ts::finally(ts::just(fragile("a")), ex::starts_on(s, ex::just_error(fragile("g")))); }, "error g", "error g");
  }
  std::printf("%-34s %d runs\n", name, runs);
  return runs;
}

using namespace ts;
template <class E, class L, class En, class D> int layouts(char const* name) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s, carried", name);    all<policies<E, carried_layout, En, D>>(buf);
  std::snprintf(buf, sizeof buf, "%s, overlapped", name); all<policies<E, overlapped_layout, En, D>>(buf);
  std::snprintf(buf, sizeof buf, "%s, scoped", name);     all<policies<E, scoped_layout, En, D>>(buf);
  return 0;
}

int main() {
#if PART == 0
  layouts<eager_teardown, void, connect_entry, stored_sender>("eager stored");
  layouts<eager_teardown, void, connect_entry, split_sender>("eager split");
#elif PART == 1
  layouts<forward_results, void, connect_entry, stored_sender>("forward stored");
  layouts<forward_results, void, connect_entry, split_sender>("forward split");
#elif PART == 2
  layouts<eager_teardown, void, start_entry, stored_sender>("eager+start stored");
  layouts<eager_teardown, void, start_entry, split_sender>("eager+start split");
#else
  layouts<forward_results, void, start_entry, stored_sender>("forward+start stored");
  layouts<forward_results, void, start_entry, split_sender>("forward+start split");
#endif
}

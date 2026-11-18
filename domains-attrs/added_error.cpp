// Build: g++ -std=c++20 -DCASE=1 -I<stdexec>/include added_error.cpp (CASE=0: no upon_error)
// Mimics nvexec's stream_domain rewriting schedule_from: the rewritten
// sender declares an extra error that the original need not declare.
#include <stdexec/execution.hpp>
#include <exec/completion_signatures.hpp>
#include <exec/sender_for.hpp>
#include <utility>
#include <cstdio>
#include <typeinfo>

namespace ex = stdexec;

struct gpu_error { int code; };
struct gpu_scheduler;

template <class Sndr, class Env>
concept gpu_completing = ex::sender_in<Sndr, Env> && requires(Sndr const& s, Env const& e) {
  { ex::get_completion_scheduler<ex::set_value_t>(ex::get_env(s), e) } -> std::same_as<gpu_scheduler>;
};

// What nvexec's schedule_from_sender does to the signatures: keep the
// child's and add set_error_t(cudaError_t) and set_stopped_t().
template <class Child>
struct gpu_schedule_from {
  using sender_concept = ex::sender_t;
  Child child_;
  template <class Self, class... Env>
  static consteval auto get_completion_signatures() {
    return exec::concat_completion_signatures(
      ex::get_completion_signatures<ex::__copy_cvref_t<Self, Child>, Env...>(),
      ex::completion_signatures<ex::set_error_t(gpu_error), ex::set_stopped_t()>());
  }
  // Like nvexec's, the operation can fail with the added error.
  template <class Rcvr>
  struct op {
    ex::connect_result_t<Child, Rcvr> child_op_;
    Rcvr* rcvr_;
    bool fail_;
    void start() & noexcept {
      if (fail_) {
        ex::set_error(std::move(*rcvr_), gpu_error{1});
      } else {
        ex::start(child_op_);
      }
    }
  };
  template <class Rcvr>
  struct holder {
    Rcvr rcvr_;
    op<Rcvr> op_;
    holder(Child c, Rcvr r, bool f) : rcvr_(std::move(r)), op_{ex::connect(std::move(c), Rcvr(rcvr_)), &rcvr_, f} {}
    void start() & noexcept { std::puts("gpu_schedule_from started"); op_.start(); }
  };
  template <class Rcvr>
  auto connect(Rcvr rcvr) && { return holder<Rcvr>(std::move(child_), std::move(rcvr), fail); }
  static inline bool fail = false;
  auto get_env() const noexcept { return ex::__fwd_env(ex::get_env(child_)); }
};

struct gpu_domain {
  template <exec::sender_for<ex::schedule_from_t> Sndr, class Env>
    requires gpu_completing<ex::__child_of<Sndr>, Env>
  static auto transform_sender(ex::set_value_t, Sndr&& sndr, Env const&) {
    auto&& [tag, data, child] = sndr;
    return gpu_schedule_from<std::decay_t<decltype(child)>>{child};
  }
};

struct gpu_scheduler {
  using scheduler_concept = ex::scheduler_t;
  template <class Rcvr>
  struct op {
    Rcvr rcvr_;
    void start() & noexcept { ex::set_value(std::move(rcvr_)); }
  };
  struct sender {
    using sender_concept = ex::sender_t;
    // As nvexec's stream scheduler's schedule sender declares.
    using completion_signatures =
      ex::completion_signatures<ex::set_value_t(), ex::set_error_t(gpu_error)>;
    template <class Rcvr>
    auto connect(Rcvr rcvr) const -> op<Rcvr> { return {std::move(rcvr)}; }
    struct attrs {
      auto query(ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept { return gpu_scheduler{}; }
      auto query(ex::get_completion_domain_t<ex::set_value_t>) const noexcept { return gpu_domain{}; }
    };
    auto get_env() const noexcept { return attrs{}; }
  };
  auto schedule() const noexcept { return sender{}; }
  auto query(ex::get_completion_domain_t<ex::set_value_t>) const noexcept { return gpu_domain{}; }
  bool operator==(gpu_scheduler const&) const = default;
};

struct sink {
  using receiver_concept = ex::receiver_t;
  template <class... As> void set_value(As&&...) && noexcept { std::puts("sink: set_value"); }
  template <class E> void set_error(E&&) && noexcept { std::printf("sink: set_error(%s)\n", typeid(E).name()); }
  void set_stopped() && noexcept {}
};

struct handler { void operator()(gpu_error) const noexcept {} };

int main() {
  ex::run_loop loop;
  auto cpu = loop.get_scheduler();
  gpu_scheduler gpu;
#if CASE == 0
  auto s = ex::schedule(gpu) | ex::continues_on(cpu);
#else
  auto s = ex::schedule(gpu) | ex::upon_error(handler{}) | ex::continues_on(cpu);
#endif
  // Is the rewrite applied to the schedule_from layer at all?
  auto sf = ex::schedule_from(ex::schedule(gpu)
#if CASE != 0
                              | ex::upon_error(handler{})
#endif
  );
  using t = decltype(ex::transform_sender(std::move(sf), ex::env<>{}));
  static_assert(!std::is_same_v<std::remove_cvref_t<t>, decltype(sf)>, "domain did not rewrite schedule_from");
  gpu_schedule_from<decltype(ex::schedule(gpu))>::fail = true;
  gpu_schedule_from<decltype(ex::schedule(gpu) | ex::upon_error(handler{}))>::fail = true;
  auto op = ex::connect(std::move(s), sink{});
  ex::start(op);
  loop.finish();
  loop.run();
}

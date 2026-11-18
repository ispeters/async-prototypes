#include "associate.hpp"
#include "just.hpp"
#include "then.hpp"
#include "repeat_n.hpp"
#include "when_all.hpp"
#include <cstdio>
#include <utility>
namespace ex = stdexec; namespace ts = frames::typestate;
struct scope_t;
scope_t* g;
struct scope_t {
  bool armed = false; int live = 0; bool closed = false; bool refusing = false; int bad = 0; int destroyed = 0; bool throwing = true;
  class association { public: association() = default; explicit association(scope_t* s) noexcept : s_(s) { if (s_) ++s_->live; }
    association(association&& o) noexcept : s_(std::exchange(o.s_, nullptr)) {}
    auto operator=(association o) noexcept -> association& { std::swap(s_, o.s_); return *this; }
    ~association() { if (s_) --s_->live; }
    explicit operator bool() const noexcept { return s_ != nullptr; }
    auto try_associate() const noexcept -> association { return association(s_ && !s_->closed ? s_ : nullptr); }
   private: scope_t* s_ = nullptr; };
  struct token {
    auto try_associate() const noexcept -> association { if (s->closed) { s->refusing = true; return association(); } return association(s); }
    template <class S> auto wrap(S&& x) const noexcept -> S&& { return static_cast<S&&>(x); } scope_t* s; };
};
// A sender that may only be destroyed while associated, or while its scope refuses.
struct checked {
  using sender_concept = ex::sender_t;
  using completion_signatures = ex::completion_signatures<ex::set_value_t(), ex::set_error_t(std::exception_ptr)>;
  bool tracked = false;
  checked() = default;
  checked(checked const&) : tracked(g->armed) {}
  checked(checked&&) noexcept : tracked(g->armed) {}
  ~checked() { if (!tracked) return; ++g->destroyed; if (g->live == 0 && !g->refusing) ++g->bad; }
  template <class R> struct op { R r; void start() & noexcept { ex::set_value(std::move(r)); } };
  template <class R> auto connect(R r) && -> op<R> { if (g->throwing) throw 42; return {std::move(r)}; }
  template <class R> auto connect(R r) const& -> op<R> { if (g->throwing) throw 42; return {std::move(r)}; }
};

struct sink { using receiver_concept = ex::receiver_t; int* errs;
  void set_value() && noexcept {} void set_error(std::exception_ptr) && noexcept { ++*errs; } void set_stopped() && noexcept {} };
template <class P> struct prcv : sink { auto get_env() const noexcept { return ts::policies_env<P>(); } };
template <class P, class Make> void check(char const* name, Make make) {
  scope_t sc; g = &sc; int errs = 0, thrown = 0;
  { checked c; sc.armed = true; auto s = make(std::move(c), scope_t::token{&sc});
    try { auto op = ex::connect(std::move(s), prcv<P>{{&errs}}); ex::start(op); } catch (int) { ++thrown; } }
  sc.armed = false;
  std::printf("%-26s thrown %d errors %d destroyed %d bad %d, live at end %d\n", name, thrown, errs, sc.destroyed, sc.bad, sc.live);
}
template <class P> void pol(char const* name) {
  check<P>(name, [](checked c, auto t) { return ts::associate(std::move(c), t); });
  char b[64]; std::snprintf(b, 64, "%s, repeat_n x3", name);
  check<P>(b, [](checked c, auto t) { return ts::repeat_n(ts::associate(std::move(c), t), 3); });
}
int main() {
  using namespace ts;
  pol<policies<eager_teardown, carried_layout, connect_entry, stored_sender>>("eager");
  pol<policies<forward_results, carried_layout, connect_entry, stored_sender>>("forward");
  pol<policies<eager_teardown, carried_layout, start_entry, stored_sender>>("eager+start");
  pol<policies<eager_teardown, carried_layout, connect_entry, split_sender>>("eager+split");
  pol<policies<forward_results, carried_layout, start_entry, split_sender>>("fwd+start+split");
}

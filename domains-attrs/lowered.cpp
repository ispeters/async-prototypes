#include "common.hpp"
#include <optional>

int main() {
  tagged<1> s1; tagged<2> s2;
  auto f = [] {};
  report("just()", ex::just());
  report("schedule(S1)", ex::schedule(s1));
  report("continues_on(just(), S1)", ex::continues_on(ex::just(), s1));
  report("starts_on(S1, just())", ex::starts_on(s1, ex::just()));
  report("on(S1, just())", ex::on(s1, ex::just()));
  report("on(schedule(S2), S1, then(f))", ex::on(ex::schedule(s2), s1, ex::then(f)));
  report("then(on(S1, just()), f)", ex::then(ex::on(s1, ex::just()), f));
  report("let_value(just(), ->schedule(S1))", ex::let_value(ex::just(), [=] { return ex::schedule(s1); }));
  report("let_value(schedule(S2), ->just())", ex::let_value(ex::schedule(s2), [] { return ex::just(); }));
  report("when_all(schedule(S1), schedule(S1))", ex::when_all(ex::schedule(s1), ex::schedule(s1)));
  report("when_all(schedule(S1), schedule(S2))", ex::when_all(ex::schedule(s1), ex::schedule(s2)));
  report("when_all_with_variant(schedule(S1))", ex::when_all_with_variant(ex::schedule(s1)));
  report("stopped_as_optional(schedule(S1)|then(->1))", ex::stopped_as_optional(ex::schedule(s1) | ex::then([] { return 1; })));
  report("stopped_as_error(schedule(S1), 1)", ex::stopped_as_error(ex::schedule(s1), 1));
  report("bulk(schedule(S1), par, 2, fn)", ex::bulk(ex::schedule(s1), ex::par, 2, [](int) {}));
}

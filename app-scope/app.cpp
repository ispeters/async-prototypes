// What does sync_wait(when_all(the_app(), scope.join())) do today?
#include <stdexec/execution.hpp>
#include <cstdio>
namespace ex = stdexec;

int main(int argc, char** argv) {
  bool wrap = argc > 1 && argv[1][0] == 'a';   // "a": wrap the app in associate
  ex::simple_counting_scope scope;
  int spawned_ran = 0;

  // "The app": suspends once (onto sync_wait's own loop), then spawns dynamic
  // work into the programme-wide scope.
  auto the_app = ex::read_env(ex::get_scheduler)
    | ex::let_value([&](auto sch) {
        return ex::schedule(sch) | ex::then([&] {
          std::puts("app: running, spawning work into the scope");
          ex::spawn(ex::just() | ex::then([&]() noexcept { ++spawned_ran; }), scope.get_token());
        });
      });

  std::puts(wrap ? "variant: when_all(associate(the_app, token), scope.join())"
                 : "variant: when_all(the_app, scope.join())");
  if (wrap)
    ex::sync_wait(ex::when_all(ex::associate(std::move(the_app), scope.get_token()), scope.join()));
  else
    ex::sync_wait(ex::when_all(std::move(the_app), scope.join()));
  std::printf("sync_wait returned; spawned work ran %d time(s)\n", spawned_ran);
}

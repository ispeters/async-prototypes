// Checks for every mode: results, errors, references, several values and
// several value completions, every channel of then and let, when_all,
// stopping, a loop's stack depth, an erased boundary, completion signatures,
// and (TYPED and VARIANT modes) which handle types come back.
#include "algos.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)

using namespace th;
using std::string;

struct pinned {
    int v;
    explicit pinned(int x) : v(x) {}
    pinned(pinned&&) = delete;
};

// A test leaf with two value completions: an int or a string.
struct pick_sender {
    bool first;
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completion_signatures<set_value_t(int), set_value_t(string)>();
    }
    template <class R>
    struct op {
        R              rcvr;
        bool           first;
        TH_INLINE auto start() & noexcept {
            return either(
                first,
                [&] { return th::set_value(rcvr, 1); },
                [&] { return th::set_value(rcvr, string("one")); });
        }
    };
    template <class R>
    op<R> connect(R rcvr) && noexcept {
        return {rcvr, first};
    }
};

template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};

// The extent of the machine stack seen from inside a loop's body.
char*                  stack_lo = nullptr;
char*                  stack_hi = nullptr;
[[gnu::noinline]] void probe() {
    char* p = static_cast<char*>(__builtin_frame_address(0));
    if (!stack_lo || p < stack_lo) {
        stack_lo = p;
    }
    if (!stack_hi || p > stack_hi) {
        stack_hi = p;
    }
}

const char* mode_name() {
    switch (TH_MODE) {
    case TH_EAGER:
        return "EAGER";
    case TH_ERASED:
        return "ERASED";
    case TH_TYPED:
        return "TYPED";
    default:
        return "VARIANT";
    }
}

template <class S, class Env = empty_env>
using sigs_t = completions_of_t<S, Env>;
template <class A, class B>
inline constexpr bool same_sigs = A() == B();

int main() {
    auto inc      = [](int x) noexcept { return x + 1; };
    using int_sig = set_value_t(int);

    { // then, and a chain of four
        run_root(then(just(1), inc), [](auto& st) {
            CHECK(st.template has<int_sig>() && st.template get<int_sig>() == 2);
        });
        run_root(then(then(then(then(just(1), inc), inc), inc), inc),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 5); });
    }
    { // a function that may throw, and does
        auto boom = [](int x) -> int {
            if (x > 0) {
                throw std::runtime_error("boom");
            }
            return x;
        };
        run_root(then(just(1), boom), [](auto& st) {
            CHECK(st.template has<set_error_t(std::exception_ptr)>());
        });
        run_root(then(just(0), boom),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 0); });
    }
    { // an immovable value, constructed where the root keeps it
        run_root(then(just(7), [](int x) noexcept { return pinned(x); }),
                 [](auto& st) { CHECK(st.template get<set_value_t(pinned)>().v == 7); });
    }
    { // void results, and several values in one completion
        static_assert(same_sigs<sigs_t<decltype(then(just(1), [](int) noexcept {}))>,
                                completion_signatures<set_value_t()>>);
        run_root(then(just(1), [](int) noexcept {}),
                 [](auto& st) { CHECK(st.template has<set_value_t()>()); });
        run_root(then(just(2, string("ab")),
                      [](int n, string s) noexcept { return s.size() * n; }),
                 [](auto& st) { CHECK(st.template get<set_value_t(size_t)>() == 4); });
        using two = set_value_t(int, string);
        run_root(just(3, string("x")), [](auto& st) {
            CHECK(st.template get<two>().template get<0>() == 3);
            CHECK(st.template get<two>().template get<1>() == "x");
        });
    }
    { // a reference completion: the next then sees the same object
        int  x   = 41;
        auto ref = then(just(&x), [](int* p) noexcept -> int& { return *p; });
        static_assert(
            same_sigs<sigs_t<decltype(ref)>, completion_signatures<set_value_t(int&)>>);
        run_root(
            then(std::move(ref), [&x](int& r) noexcept { return &r == &x ? ++r : -1; }),
            [](auto& st) { CHECK(st.template get<int_sig>() == 42); });
        CHECK(x == 42);
    }
    { // two value completions through then
        auto f = overloaded{[](int i) noexcept { return i * 10; },
                            [](string s) noexcept { return s + "!"; }};
        static_assert(
            same_sigs<sigs_t<decltype(then(pick_sender{true}, f))>,
                      completion_signatures<set_value_t(int), set_value_t(string)>>);
        run_root(then(pick_sender{true}, f),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 10); });
        run_root(then(pick_sender{false}, f), [](auto& st) {
            CHECK(st.template get<set_value_t(string)>() == "one!");
        });
    }
    { // upon_error and upon_stopped
        run_root(upon_error(just_error(5), [](int e) noexcept { return e * 2; }),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 10); });
        run_root(upon_stopped(just_stopped(), []() noexcept { return 3; }),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 3); });
        run_root(upon_error(just(1), [](int e) noexcept { return e; }), // passes
                 [](auto& st) { CHECK(st.template get<int_sig>() == 1); });
    }
    { // let_value, the second child borrowing the first's value
        run_root(
            let_value(just(string("ab")),
                      [](string& s) noexcept { return just(s + "c"); }),
            [](auto& st) { CHECK(st.template get<set_value_t(string)>() == "abc"); });
        // one second sender per value completion
        auto g = overloaded{[](int& i) noexcept { return just(i + 1); },
                            [](string& s) noexcept { return just(s.size()); }};
        run_root(let_value(pick_sender{true}, g),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 2); });
        run_root(let_value(pick_sender{false}, g),
                 [](auto& st) { CHECK(st.template get<set_value_t(size_t)>() == 3); });
        // a reference completion is copied before the first child goes
        int x = 5;
        run_root(let_value(then(just(&x), [](int* p) noexcept -> int& { return *p; }),
                           [&x](int& r) noexcept { return just(&r != &x && r == 5); }),
                 [](auto& st) { CHECK(st.template get<set_value_t(bool)>()); });
        // a function that throws
        run_root(
            let_value(just(1), [](int&) -> just_sender<set_value_t, int> { throw 1; }),
            [](auto& st) { CHECK(st.failed()); });
    }
    { // let_error and let_stopped
        run_root(let_error(just_error(string("e")),
                           [](string& e) noexcept { return just(e + "!"); }),
                 [](auto& st) { CHECK(st.template get<set_value_t(string)>() == "e!"); });
        run_root(let_stopped(just_stopped(), []() noexcept { return just(9); }),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 9); });
    }
    { // when_all: two children, then three with several and borrowed values
        run_root(when_all(then(just(1), inc), just(string("x"))), [](auto& st) {
            using sig = set_value_t(int, string);
            CHECK(st.template has<sig>());
            CHECK(st.template get<sig>().template get<0>() == 2);
            CHECK(st.template get<sig>().template get<1>() == "x");
        });
        int  y     = 7;
        auto three = when_all(just(1, 'c'),
                              then(just(&y), [](int* p) noexcept -> int& { return *p; }),
                              just(string("z")));
        using sig3 = set_value_t(int, char, int, string);
        static_assert(same_sigs<sigs_t<decltype(three)>,
                                completion_signatures<sig3, set_stopped_t()>>);
        run_root(std::move(three), [&y](auto& st) {
            auto& v = st.template get<sig3>();
            CHECK(v.template get<0>() == 1 && v.template get<1>() == 'c');
            CHECK(v.template get<2>() == 7 && &v.template get<2>() != &y); // a copy
            CHECK(v.template get<3>() == "z");
        });
        // errors of different types, decayed and handed on
        auto boom = [](int) -> int { throw 1; };
        run_root(when_all(then(just(1), boom), just(2)), [](auto& st) {
            CHECK(st.template has<set_error_t(std::exception_ptr)>());
        });
        run_root(when_all(just_error(string("bad")), just(2)), [](auto& st) {
            CHECK(st.template get<set_error_t(string)>() == "bad");
        });
        // both fail: the first is handed on, the second destroyed
        run_root(
            when_all(just_error(string("first")), just_error(string("second"))),
            [](auto& st) { CHECK(st.template get<set_error_t(string)>() == "first"); });
    }
    { // an erased boundary in the middle of a chain, and with two completions
        run_root(then(erase_boundary(then(just(1), inc)), inc),
                 [](auto& st) { CHECK(st.template get<int_sig>() == 3); });
        run_root(erase_boundary(pick_sender{false}), [](auto& st) {
            CHECK(st.template get<set_value_t(string)>() == "one");
        });
    }

    // Stop tokens.
    {
        static_assert(same_sigs<sigs_t<decltype(just_unless_stopped(1))>,
                                completion_signatures<int_sig>>);
        static_assert(same_sigs<sigs_t<decltype(just_unless_stopped(1)),
                                       prop<get_stop_token_t, inplace_stop_token>>,
                                completion_signatures<int_sig, set_stopped_t()>>);
        inplace_stop_source live;
        run_root(
            then(just_unless_stopped(1), inc),
            [](auto& st) { CHECK(st.template get<int_sig>() == 2); },
            live.get_token());
        inplace_stop_source stopped;
        stopped.request_stop();
        run_root(
            then(just_unless_stopped(1), inc),
            [](auto& st) { CHECK(st.stopped()); },
            stopped.get_token());
        run_root(then(just_unless_stopped(1), inc), // no token at all
                 [](auto& st) { CHECK(st.template get<int_sig>() == 2); });
    }
    { // a child's failure stops its sibling, through when_all's own source
        bool ran  = false;
        auto boom = [](int) -> int { throw 1; };
        run_root(when_all(then(just(1), boom),
                          then(just_unless_stopped(2),
                               [&ran](int x) noexcept {
                                   ran = true;
                                   return x;
                               })),
                 [](auto& st) { CHECK(st.failed()); });
        CHECK(!ran);
    }
    { // the parent's stop reaches when_all's children, through a callback
        inplace_stop_source stopped;
        stopped.request_stop();
        run_root(
            when_all(just_unless_stopped(1), just_unless_stopped(2)),
            [](auto& st) { CHECK(st.stopped()); },
            stopped.get_token());
        inplace_stop_source live; // registered, then deregistered
        run_root(
            when_all(just_unless_stopped(1), just_unless_stopped(2)),
            [](auto& st) {
                CHECK((st.template get<set_value_t(int, int)>().template get<0>() == 1));
            },
            live.get_token());
        CHECK(!live.stop_requested());
    }
    { // through an erased boundary, by its one forwarded query
        inplace_stop_source stopped;
        stopped.request_stop();
        run_root(
            then(erase_boundary(then(just_unless_stopped(1), inc)), inc),
            [](auto& st) { CHECK(st.stopped()); },
            stopped.get_token());
        inplace_stop_source live;
        run_root(
            then(erase_boundary(then(just_unless_stopped(1), inc)), inc),
            [](auto& st) { CHECK(st.template get<int_sig>() == 3); },
            live.get_token());
    }
    { // a loop stopped from inside its body
        inplace_stop_source src;
        int                 count = 0;
        run_root(
            repeat_until(then(just_unless_stopped(&count),
                              [&src](int* c) noexcept {
                                  if (++*c == 5) {
                                      src.request_stop();
                                  }
                                  return false;
                              })),
            [](auto& st) { CHECK(st.stopped()); },
            src.get_token());
        CHECK(count == 5);
    }
    { // a loop whose child's value converts to bool, and one that fails
        int count = 0;
        run_root(repeat_until(then(just(&count),
                                   [](int* c) noexcept { return ++*c == 3 ? 1 : 0; })),
                 [](auto& st) { CHECK(st.template has<set_value_t()>()); });
        CHECK(count == 3);
        run_root(repeat_until(just_error(4)),
                 [](auto& st) { CHECK(st.template get<set_error_t(int)>() == 4); });
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
                 [](auto& st) { CHECK(st.template has<set_value_t()>()); });
        CHECK(count == n);
        extent[i++] = long(stack_hi - stack_lo);
        std::printf("%s repeat_until %4d iterations: stack extent %ld bytes\n",
                    mode_name(),
                    n,
                    extent[i - 1]);
    }
#if TH_MODE != TH_EAGER
    CHECK(extent[0] == extent[1]); // bounded: the same whatever n is
#endif

    // Which handles come back. In TYPED mode a then whose function can't throw
    // hands its parent a typed handle; one that can throw has two outcomes, so
    // its completion is erased, or in VARIANT mode a choice of the two.
    {
        using Sigs = completion_signatures<int_sig>;
        using R    = root_rcvr<Sigs>;
        using nothrow_op =
            then_op<set_value_t, just_sender<set_value_t, int>, decltype(inc), R>;
        using start_t_ = decltype(std::declval<nothrow_op&>().start());
        using complete_t_ =
            decltype(std::declval<nothrow_op&>().template invoke<int_sig>());
#if TH_MODE == TH_TYPED || TH_MODE == TH_VARIANT
        static_assert(
            std::same_as<start_t_, operation_handle<nothrow_op::rcvr_t, int_sig>>);
        static_assert(std::same_as<complete_t_, operation_handle<R, int_sig>>);
        auto may_throw = [](int x) { return x; };
        using R2 =
            root_rcvr<completion_signatures<int_sig, set_error_t(std::exception_ptr)>>;
        using throwing_op =
            then_op<set_value_t, just_sender<set_value_t, int>, decltype(may_throw), R2>;
        using throwing_t =
            decltype(std::declval<throwing_op&>().template invoke<int_sig>());
    #if TH_MODE == TH_TYPED
        static_assert(std::same_as<throwing_t, erased_handle>);
    #else // a choice of the two completions
        static_assert(
            std::same_as<throwing_t,
                         choice<operation_handle<R2, int_sig>,
                                operation_handle<R2, set_error_t(std::exception_ptr)>>>);
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

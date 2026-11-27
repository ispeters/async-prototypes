// Checks for every mode: results, errors, references, several values and
// several value completions, every channel of then and let, when_all,
// stopping, a loop's stack depth, an erased boundary, completion signatures,
// and (TYPED and VARIANT modes) which handle types come back.
#include "algos.hpp"
#include "sched.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

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
        return {std::move(rcvr), first};
    }
};

// Counts the instances alive, to show what a throw leaves behind.
struct counted {
    inline static int live = 0;
    counted() noexcept { ++live; }
    counted(counted&&) noexcept { ++live; }
    ~counted() { --live; }
};
// Moving it throws.
struct flaky {
    flaky() = default;
    flaky(flaky&&) { throw std::runtime_error("flaky"); }
};
// A leaf completing with three values whose middle one throws as it is
// moved into place.
struct throw_mid_sender {
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completion_signatures<set_value_t(counted, flaky, counted),
                                     set_error_t(std::exception_ptr)>();
    }
    template <class R>
    struct op {
        R              rcvr;
        TH_INLINE auto start() & noexcept {
            return th::set_value(rcvr, counted(), flaky(), counted());
        }
    };
    template <class R>
    op<R> connect(R rcvr) && noexcept {
        return {std::move(rcvr)};
    }
};

// A leaf that declares an int value but delivers an int error.
struct int_error_sender {
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completion_signatures<set_value_t(int), set_error_t(int)>();
    }
    template <class R>
    struct op {
        R              rcvr;
        TH_INLINE auto start() & noexcept { return th::set_error(rcvr, 7); }
    };
    template <class R>
    op<R> connect(R rcvr) && noexcept {
        return {std::move(rcvr)};
    }
};

// A leaf that completes only when stop is requested of its receiver's token,
// on whichever thread requests it.
struct stop_waiter_sender {
    template <class Env>
    static consteval auto get_completion_signatures() {
        return completion_signatures<set_value_t(), set_stopped_t()>();
    }
    template <class R>
    struct op {
        struct on_stop {
            op*  self;
            void operator()() noexcept { th::run(th::set_stopped(self->rcvr)); }
        };
        R                                             rcvr;
        std::optional<inplace_stop_callback<on_stop>> callback;

        TH_INLINE no_operation start() & noexcept {
            callback.emplace(get_stop_token(th::get_env(rcvr)), on_stop{this});
            return {};
        }
    };
    template <class R>
    op<R> connect(R rcvr) && noexcept {
        return {std::move(rcvr), {}};
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

// A receiver that records which of many receiver types completed.
template <std::size_t I>
struct tally_rcvr : operation_handle<tally_rcvr<I>, std::size_t> {
    using base_t = operation_handle<tally_rcvr, std::size_t>;
    using base_t::base_t;

  private:
    friend base_t;

    template <class Sig>
    TH_INLINE static no_operation resume(std::size_t& self) noexcept {
        self = I;
        return {};
    }
};
template <std::size_t I>
using tally_handle = operation_handle<tally_rcvr<I>, set_value_t()>;

template <class... Hs>
constexpr std::size_t alternatives_in(type_list<Hs...>) noexcept {
    return sizeof...(Hs);
}

// Where N different typed handles meet, the common type is one choice of all
// N in VARIANT mode, and the erased handle otherwise; either way each runs.
// They all run through one function, as a merge point settles its choice in
// one place: settled at N call sites, an N-way choice is N * N branches to
// compile.
template <class Wide>
[[gnu::noinline]] void run_wide(Wide w) noexcept {
    run(w);
}
template <std::size_t... I>
void check_wide_choice(std::index_sequence<I...>) {
    using wide = common_handle_t<tally_handle<I>...>;
#if TH_MODE == TH_VARIANT
    static_assert(std::same_as<wide, choice<tally_handle<I>...>>);
    static_assert(sizeof(wide) <= 2 * sizeof(void*));
#else
    static_assert(std::same_as<wide, erased_handle>);
#endif
    // A table, not a fold over I: Clang limits a fold to 256 operands.
    using make_t                   = wide (*)(std::size_t*) noexcept;
    static constexpr make_t make[] = {[](std::size_t* ran) noexcept -> wide {
        return tally_rcvr<I>(ran).template complete<set_value_t()>();
    }...};
    for (std::size_t i = 0; i < sizeof...(I); ++i) {
        std::size_t ran = sizeof...(I);
        run_wide(make[i](&ran));
        CHECK(ran == i);
    }
}

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
    { // several values whose construction throws part-way: the elements
      // already constructed are destroyed, and the error is delivered
        run_root(throw_mid_sender{}, [](auto& st) {
            CHECK(st.template has<set_error_t(std::exception_ptr)>());
        });
        CHECK(counted::live == 0);
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
        // errors of three types: five outcomes meet where the children arrive
        run_root(
            when_all(just_error(string("s")), just_error(1), just_error(2.0), just(3)),
            [](auto& st) { CHECK(st.template get<set_error_t(string)>() == "s"); });
        run_root(
            when_all(just(3), just_error(1), just_error(2.0), just_error(string("s"))),
            [](auto& st) { CHECK(st.template get<set_error_t(int)>() == 1); });
        run_root(
            when_all(just(3), just_error(2.0), just_error(1), just_error(string("s"))),
            [](auto& st) { CHECK(st.template get<set_error_t(double)>() == 2.0); });
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

    // Where many outcomes meet. when_all with errors of three types arrives in
    // one of five ways (nothing yet, stopped, or an error of each type; never
    // values, since three of its children can't produce any); in VARIANT mode
    // that's a choice of all five. Then 257 different typed handles meeting at
    // once.
    {
        using S  = decltype(when_all(
            just_error(string("s")), just_error(1), just_error(2.0), just(3)));
        using R  = root_rcvr<completions_of_t<S, empty_env>>;
        using op = connect_result_t<S, R>;
#if TH_MODE == TH_VARIANT
        static_assert(is_choice<typename op::arrive_t>);
        static_assert(
            alternatives_in(typename alternatives<typename op::arrive_t>::type()) == 5);
#elif TH_MODE == TH_TYPED || TH_MODE == TH_ERASED
        static_assert(std::same_as<typename op::arrive_t, erased_handle>);
#endif
        // 257: more than a byte can index, and more operands than Clang allows
        // a fold expression.
        check_wide_choice(std::make_index_sequence<257>());
    }

    { // sync_wait, synchronously and from a thread pool
        auto one = sync_wait(just(1));
        CHECK(one && std::get<0>(*one) == 1);
        thread_pool pool(4);
        auto        where = sync_wait(then(schedule(pool.get_scheduler()),
                                    [] noexcept { return std::this_thread::get_id(); }));
        CHECK(where && std::get<0>(*where) != std::this_thread::get_id());
        bool caught = false;
        try {
            sync_wait(then(schedule(pool.get_scheduler()),
                           []() -> int { throw std::runtime_error("pool"); }));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
        caught = false;
        try {
            sync_wait(int_error_sender{});
        } catch (int e) {
            caught = e == 7;
        }
        CHECK(caught);
    }
    { // children completing concurrently on a pool's threads
        thread_pool pool(4);
        auto        sch   = pool.get_scheduler();
        auto        twice = [](int x) noexcept { return 2 * x; };
        for (int i = 0; i < 200; ++i) {
            auto r = sync_wait(
                when_all(then(then(schedule(sch), [] noexcept { return 1; }), twice),
                         then(then(schedule(sch), [] noexcept { return 2; }), twice),
                         then(then(schedule(sch), [] noexcept { return 3; }), twice),
                         then(then(schedule(sch), [] noexcept { return 4; }), twice)));
            CHECK(r && std::get<0>(*r) == 2 && std::get<1>(*r) == 4 &&
                  std::get<2>(*r) == 6 && std::get<3>(*r) == 8);
        }
    }
    { // stop requested from another thread
        using Sigs = completions_of_t<stop_waiter_sender, root_env_t<inplace_stop_token>>;
        inplace_stop_source                  src;
        root_state<Sigs, inplace_stop_token> st;
        st.token = src.get_token();
        auto op =
            th::connect(stop_waiter_sender{}, root_rcvr<Sigs, inplace_stop_token>(&st));
        th::run(th::start(op));
        CHECK(st.which == -1);
        std::thread([&] { src.request_stop(); }).join();
        CHECK(st.stopped());
    }
    { // a child failing on a pool's thread stops its sibling from there
        thread_pool pool(2);
        bool        caught = false;
        try {
            sync_wait(when_all(stop_waiter_sender{},
                               then(schedule(pool.get_scheduler()), []() -> int {
                                   throw std::runtime_error("sibling");
                               })));
        } catch (const std::runtime_error&) {
            caught = true;
        }
        CHECK(caught);
    }
    { // schedule's completions depend on the environment
        static_assert(same_sigs<completions_of_t<schedule_sender, empty_env>,
                                completion_signatures<set_value_t()>>);
        static_assert(
            same_sigs<completions_of_t<schedule_sender,
                                       prop<get_stop_token_t, inplace_stop_token>>,
                      completion_signatures<set_value_t(), set_stopped_t()>>);
    }

    std::printf("%s checks pass\n", mode_name());
}

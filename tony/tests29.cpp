#include "algos29.hpp"
#include "counted.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>
namespace k = c29;
using x29::operation_handle;
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)

struct imm_root_t;
struct imm_root {
    x29::slot<pinned>             v;
    x29::slot<std::exception_ptr> e;
};
struct imm_rcvr {
    imm_root* r;
    template <class Sig>
    void* destination() noexcept {
        if constexpr (std::same_as<Sig, x29::set_value_t(pinned)>)
            return r->v.address();
        else
            return r->e.address();
    }
    template <class Sig>
    operation_handle complete() noexcept {
        if constexpr (std::same_as<Sig, x29::set_value_t(pinned)>)
            r->v.adopt();
        return {};
    }
    x29::empty_env get_env() const noexcept { return {}; }
};

struct loop_rcvr {
    k::run_loop*    loop;
    x29::slot<int>* got;
    template <class Sig>
    void* destination() noexcept {
        return got->address();
    }
    template <class Sig>
    operation_handle complete() noexcept {
        if constexpr (std::same_as<Sig, x29::set_value_t(int)>)
            got->adopt();
        loop->finish();
        return {};
    }
    x29::empty_env get_env() const noexcept { return {}; }
};

int main() {
    {
        auto s = k::then(k::just(counted("hello")),
                         [](counted&& c) { return int(c.s.size()); });
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && *r == 5);
        std::printf("C++29 then(just): moves=%d\n", moves);
    }
    {
        auto s = k::then(k::just_constructing(counted("hello")),
                         [](counted&& c) { return int(c.s.size()); });
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && *r == 5);
        std::printf("C++29 then(just constructing): moves=%d\n", moves);
    }
    {
        auto s = k::just(counted("hello"));
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && r->s == "hello");
        std::printf("C++29 just: moves=%d\n", moves);
    }
    {
        auto s = k::just_constructing(counted("hello"));
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && r->s == "hello");
        std::printf("C++29 just constructing: moves=%d\n", moves);
    }
    {
        auto r = k::sync_wait(k::write_env(
            k::just(7), x29::prop<x29::get_scheduler_t, int>{x29::get_scheduler, 0}));
        CHECK(r && *r == 7);
    }
    {
        auto s = k::let_value(k::just(counted("ab")),
                              [](counted& c) { return k::just(counted(c.s + "c")); });
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && r->s == "abc");
        std::printf("C++29 let_value: moves=%d\n", moves);
    }
    {
        auto s = k::then(k::when_all(k::just(counted("x")), k::just(counted("yy"))),
                         [](x29::values<counted, counted>&& v) {
                             return int(v.first.s.size() + v.second.s.size());
                         });
        moves  = 0;
        auto r = k::sync_wait(std::move(s));
        CHECK(r && *r == 3);
        std::printf("C++29 then(when_all): moves=%d\n", moves);
        bool threw = false;
        try {
            k::sync_wait(
                k::then(k::when_all(k::just(counted("x")), k::just_error_sender<int>{}),
                        [](x29::values<counted, int>&&) { return 0; }));
        } catch (int i) {
            threw = (i == 42);
        }
        CHECK(threw);
    }
    {
        imm_root r;
        auto     op = k::then(k::just(41), [](int&& i) {
                      return pinned(i + 1);
                      }).connect(imm_rcvr{&r});
        x29::run(x29::start(op));
        CHECK(r.v.has_value() && (*r.v).v == 42);
    }
    {
        k::run_loop                           loop;
        x29::slot<int>                        got;
        k::then_void_op<int (*)(), loop_rcvr> op(
            loop.get_scheduler().schedule(), [] { return 9; }, loop_rcvr{&loop, &got});
        operation_handle h{};
        std::thread      t([&] { h = x29::start(op); });
        t.join();
        loop.run(h);
        CHECK(got.has_value() && *got == 9);
    }
#define COL "C++29"
#define SUM                                               \
    [](x29::values<counted, counted>&& v) {               \
        return int(v.first.s.size() + v.second.s.size()); \
    }
#include "fresh_body.inc"
    std::printf("C++29 checks pass\n");
}

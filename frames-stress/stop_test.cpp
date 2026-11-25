// Sandbox-only stress test of the phase-based when_all's stop handling.
// Build with -DPART=0..3 (two policy combinations each).
#include "when_all.hpp"
#include <exec/static_thread_pool.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <optional>
#include <thread>

namespace ex = stdexec;
namespace ts = frames::typestate;

// Completes with set_stopped once stop is requested; registers its callback
// carefully: whichever of start and the callback arrives second completes.
struct stop_leaf {
    using sender_concept        = ex::sender_t;
    using completion_signatures = ex::completion_signatures<ex::set_stopped_t()>;
    template <class R>
    struct op {
        struct cb_fn {
            op*  self;
            void operator()() const noexcept { self->arrive(); }
        };
        using cb_t = ex::stop_callback_for_t<ex::stop_token_of_t<ex::env_of_t<R>>, cb_fn>;
        R                   rcvr;
        std::optional<cb_t> cb;
        std::atomic<int>    pending{2};
        void                start() & noexcept {
            cb.emplace(ex::get_stop_token(ex::get_env(rcvr)), cb_fn{this});
            arrive();
        }
        void arrive() noexcept {
            if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                cb.reset();
                ex::set_stopped(std::move(rcvr));
            }
        }
    };
    template <class R>
    auto connect(R r) const -> op<R> {
        return op<R>{std::move(r)};
    }
};

struct ctx {
    ex::inplace_stop_source src;
    std::atomic<bool>       finished{false};
    void*                   op = nullptr;
    void (*del)(void*)         = nullptr;
    bool delete_in_completion  = false;
    void done() noexcept {
        if (delete_in_completion)
            del(op);
        finished.store(true, std::memory_order_release);
        finished.notify_one();
    }
};

template <class P>
struct test_env {
    ex::inplace_stop_token tok;
    auto                   query(ex::get_stop_token_t) const noexcept { return tok; }
    auto                   query(ts::get_policies_t) const noexcept -> P { return {}; }
};

template <class P>
struct rcvr {
    using receiver_concept = ex::receiver_t;
    ctx* c;
    auto get_env() const noexcept -> test_env<P> { return {c->src.get_token()}; }
    template <class... A>
    void set_value(A&&...) && noexcept {
        c->done();
    }
    template <class E>
    void set_error(E&&) && noexcept {
        c->done();
    }
    void set_stopped() && noexcept { c->done(); }
};

enum class stop_when { never, before_start, racing };

template <class P, class Sndr>
void run(Sndr sndr, stop_when when, bool delete_in_completion) {
    ctx c;
    c.delete_in_completion = delete_in_completion;
    using op_t             = ex::connect_result_t<Sndr, rcvr<P>>;
    auto* op               = new op_t(ex::connect(std::move(sndr), rcvr<P>{&c}));
    c.op                   = op;
    c.del                  = [](void* p) { delete static_cast<op_t*>(p); };
    if (when == stop_when::before_start)
        c.src.request_stop();
    std::optional<std::jthread> stopper;
    if (when == stop_when::racing)
        stopper.emplace([&] { c.src.request_stop(); });
    ex::start(*op);
    c.finished.wait(false, std::memory_order_acquire);
    stopper.reset();
    if (!delete_in_completion)
        delete op;
}

exec::static_thread_pool pool(3);

template <class P>
int all(const char* name, int n) {
    auto s = pool.get_scheduler();
    // A child on another thread that can't fail: when_all gives it no stop
    // source of its own.
    auto steady = [s](int v) {
        return ex::upon_stopped(ex::starts_on(s, ex::just(v)),
                                [v]() noexcept { return v; });
    };
    int runs = 0;
    for (int i = 0; i < n; ++i)
        for (bool del : {false, true}) {
            for (auto w : {stop_when::never, stop_when::before_start, stop_when::racing})
                run<P>(ts::when_all(
                           stop_leaf{}, ex::starts_on(s, ex::just_error(1)), stop_leaf{}),
                       w,
                       del),
                    ++runs;
            for (auto w : {stop_when::before_start, stop_when::racing}) {
                run<P>(ts::when_all(stop_leaf{}, stop_leaf{}), w, del), ++runs;
                run<P>(ts::when_all(stop_leaf{}, ex::starts_on(s, ex::just(1))), w, del),
                    ++runs;
            }
            for (auto w :
                 {stop_when::never, stop_when::before_start, stop_when::racing}) {
                run<P>(ts::when_all(ex::starts_on(s, ex::just(1)),
                                    ex::starts_on(s, ex::just(2))),
                       w,
                       del),
                    ++runs;
                run<P>(ts::when_all(steady(1), steady(2)), w, del), ++runs;
            }
        }
    std::printf("%-22s %d runs\n", name, runs);
    return runs;
}

using namespace ts;
template <class E, class En, class D>
using pol = policies<E, carried_layout, En, D>;

int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 200;
#if PART == 0
    all<pol<eager_teardown, connect_entry, stored_sender>>("eager stored", n);
    all<pol<eager_teardown, connect_entry, split_sender>>("eager split", n);
#elif PART == 1
    all<pol<forward_results, connect_entry, stored_sender>>("forward stored", n);
    all<pol<forward_results, connect_entry, split_sender>>("forward split", n);
#elif PART == 2
    all<pol<eager_teardown, start_entry, stored_sender>>("eager+start stored", n);
    all<pol<eager_teardown, start_entry, split_sender>>("eager+start split", n);
#else
    all<pol<forward_results, start_entry, stored_sender>>("forward+start stored", n);
    all<pol<forward_results, start_entry, split_sender>>("forward+start split", n);
#endif
}

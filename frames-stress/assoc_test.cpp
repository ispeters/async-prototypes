// Sandbox-only test of the phase-based associate under every policy: work
// on a thread pool associated with a counting scope, joined afterwards; a
// closed scope; the receiver destroying the operation inside its completion.
// Build with -DPART=0..3.
#include "associate.hpp"
#include "just.hpp"
#include "then.hpp"
#include "when_all.hpp"
#include <exec/static_thread_pool.hpp>

#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>

namespace ex = stdexec;
namespace ts = frames::typestate;

exec::static_thread_pool pool(3);

struct ctx {
    std::atomic<bool> finished{false};
    void*             op = nullptr;
    void (*del)(void*)   = nullptr;
    int  got             = -1; // 0 value, 1 stopped, 2 error
    void done(int g) noexcept {
        got = g;
        del(op);
        finished.store(true, std::memory_order_release);
        finished.notify_one();
    }
};
template <class P>
struct rcvr {
    using receiver_concept = ex::receiver_t;
    ctx* c;
    auto get_env() const noexcept -> ts::policies_env<P> { return {}; }
    template <class... A>
    void set_value(A&&...) && noexcept {
        c->done(0);
    }
    void set_stopped() && noexcept { c->done(1); }
    template <class E>
    void set_error(E&&) && noexcept {
        c->done(2);
    }
};
template <class P, class S>
int run(S sndr) {
    ctx c;
    using op_t = ex::connect_result_t<S, rcvr<P>>;
    auto* op   = new op_t(ex::connect(std::move(sndr), rcvr<P>{&c}));
    c.op       = op;
    c.del      = [](void* p) { delete static_cast<op_t*>(p); };
    ex::start(*op);
    c.finished.wait(false, std::memory_order_acquire);
    return c.got;
}

template <class P>
void all(const char* name, int n) {
    auto             s = pool.get_scheduler();
    std::atomic<int> ran{0};
    for (int i = 0; i < n; ++i) {
        ex::simple_counting_scope scope;
        auto                      work = [&] {
            return ts::then(
                ts::associate(ex::starts_on(s, ex::just()), scope.get_token()),
                [&] { ++ran; });
        };
        if (run<P>(ts::when_all(work(), work(), work())) != 0)
            std::abort();
        if (run<P>(ts::associate(ex::starts_on(s, ex::just_error(1)),
                                 scope.get_token())) != 2)
            std::abort();
        ex::sync_wait(scope.join());
        scope.close();
        if (run<P>(ts::associate(ex::starts_on(s, ex::just()), scope.get_token())) != 1)
            std::abort();
    }
    if (ran != 3 * n)
        std::abort();
    std::printf("%-22s %d iterations\n", name, n);
}

using namespace ts;
template <class E, class En, class D>
using pol = policies<E, carried_layout, En, D>;
int main(int argc, char** argv) {
    int n = argc > 1 ? std::atoi(argv[1]) : 500;
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

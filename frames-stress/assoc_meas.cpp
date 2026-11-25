#include "associate.hpp"
#include "just.hpp"
#include "repeat_n.hpp"
#include "then.hpp"
#include <exec/static_thread_pool.hpp>

#include <cstdio>
namespace ex = stdexec;
namespace ts = frames::typestate;
static int moves, copies, alive;
struct counted {
    char pad[64]{};
    counted() { ++alive; }
    counted(const counted&) {
        ++copies;
        ++alive;
    }
    counted(counted&&) noexcept {
        ++moves;
        ++alive;
    }
    ~counted() { --alive; }
};
// a plain (stdexec-style) sender carrying a counted payload
struct plain {
    using sender_concept        = ex::sender_t;
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    counted c;
    template <class R>
    struct op {
        R    r;
        void start() & noexcept { ex::set_value(std::move(r)); }
    };
    template <class R>
    auto connect(R r) && -> op<R> {
        return {std::move(r)};
    }
    template <class R>
    auto connect(R r) const& -> op<R> {
        return {std::move(r)};
    }
};
struct fn_t {
    counted c;
    void    operator()() const noexcept {}
};
struct rcvr_base {
    using receiver_concept = ex::receiver_t;
    void set_value() && noexcept {}
    void set_stopped() && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}
};
template <class P>
struct rcvr : rcvr_base {
    auto get_env() const noexcept { return ts::policies_env<P>(); }
};
template <class P, class S>
void run(const char* name, S s) {
    moves = copies = 0;
    {
        auto op = ex::connect(std::move(s), rcvr<P>{});
        ex::start(op);
        std::printf("  %-12s %-28s size %4zu  moves %d copies %d\n",
                    P::name,
                    name,
                    sizeof(op),
                    moves,
                    copies);
    }
}
#define POL(N, E, En, D)                                                \
    struct N : ts::policies<ts::E, ts::carried_layout, ts::En, ts::D> { \
        static constexpr char const* name = #N;                         \
    };
POL(eager, eager_teardown, connect_entry, stored_sender)
POL(forward, forward_results, connect_entry, stored_sender)
POL(eager_start, eager_teardown, start_entry, stored_sender)
POL(eager_split, eager_teardown, connect_entry, split_sender)
POL(fwd_start_split, forward_results, start_entry, split_sender)
template <class P>
void all(ex::simple_counting_scope& scope) {
    auto tok = scope.get_token();
    {
        plain p;
        moves = copies = 0;
        run<P>("associate(plain)", ts::associate(std::move(p), tok));
    }
    {
        fn_t f;
        run<P>("associate(then(just(),fn))",
               ts::associate(ts::then(ts::just(), std::move(f)), tok));
    }
    {
        plain p;
        run<P>("repeat_n(assoc(plain),3)",
               ts::repeat_n(ts::associate(std::move(p), tok), 3));
    }
    {
        fn_t f;
        run<P>("repeat_n(assoc(then),3)",
               ts::repeat_n(ts::associate(ts::then(ts::just(), std::move(f)), tok), 3));
    }
}
int main() {
    ex::simple_counting_scope scope;
    all<eager>(scope);
    all<forward>(scope);
    all<eager_start>(scope);
    all<eager_split>(scope);
    all<fwd_start_split>(scope);
    ex::sync_wait(scope.join());
    std::printf("alive %d\n", alive);
}

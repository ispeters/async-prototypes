// Negative test of phased::check_entry's split_sender assertion: an
// operation that enters a phase other than First, where that phase connects
// a part First connects on entry, must not compile under split_sender with
// connect_entry. Build with
//   -DDATA=split_sender  -DENTER_SECOND=1  -> must fail with
//      "a part the first phase connects on entry isn't kept for another phase"
//   -DDATA=split_sender  -DENTER_SECOND=0  -> compiles (control)
//   -DDATA=stored_sender -DENTER_SECOND=1  -> compiles (control)
#include "just.hpp"
#include "typestate.hpp"

#include <stdexec/execution.hpp>

#include <cstdio>

namespace ex = stdexec;
namespace ts = frames::typestate;

template <class Sndr>
struct twin_sender : ts::phased_sender<twin_sender<Sndr>> {
    Sndr sndr_;

    template <class Self, class... Env>
    static consteval auto get_completion_signatures() {
        if constexpr (sizeof...(Env) == 0) {
            return STDEXEC::__throw_dependent_sender_error<Self>();
        } else {
            return exec::concat_completion_signatures(
                ex::get_completion_signatures<ts::member_t<Self, Sndr>, Env...>(),
                ex::completion_signatures<ex::set_error_t(std::exception_ptr)>());
        }
    }

    template <class Rcvr, bool Const = false>
    struct opstate {
        struct tag {};

        using parts_t = ts::parts<&twin_sender::sndr_>;
        using rcvr_t  = ts::phase_receiver<opstate, Rcvr, tag, &twin_sender::sndr_>;
        using child_t = ts::part_child_t<ts::maybe_const<Const, Sndr>, rcvr_t>;

        template <int>
        struct phase {
            static constexpr auto connects = &twin_sender::sndr_;

            template <class... Incoming>
            explicit phase(opstate* op, Incoming&... incoming)
                : child(op->phases_.template connect_part<connects>(rcvr_t{op},
                                                                    incoming...)) {}

            child_t child;
        };

        using first  = phase<1>;
        using second = phase<2>;
        using entry  = std::conditional_t<ENTER_SECOND, second, first>;

        template <class... Incoming>
        explicit opstate(Rcvr rcvr, Incoming&... incoming)
            : phases_(static_cast<Rcvr&&>(rcvr), ts::nothing(), incoming...) {
            phases_.template on_connect<entry>(this, incoming...);
        }

        void start() & noexcept { phases_.template begin<entry>(ts::start_child, this); }

        template <class Tag, class... As>
        void complete(tag, Tag, As&&... as) noexcept {
            phases_.template finish<entry>(Tag(), static_cast<As&&>(as)...);
        }

        ts::phased<Rcvr, parts_t, ts::nothing, first, second> phases_;
    };
};

int main() {
    using P =
        ts::policies<ts::eager_teardown, ts::carried_layout, ts::connect_entry, ts::DATA>;
    auto [v] = ex::sync_wait(ts::with_policies<P>(
                                 twin_sender<decltype(ts::just(42))>{{}, ts::just(42)}))
                   .value();
    std::printf("%d\n", v);
}

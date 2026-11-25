// What a parent does with stdexec's let_value attributes, which forward the
// first child's even though the operation completes where the function's
// sender does.
#include "common.hpp"

// dom<7> customizes then: it records that it was chosen and leaves the sender
// as it is.
inline int then_customized = 0;
template <>
struct dom<7> {
    template <exec::sender_for<ex::then_t> Sndr, class Env>
    auto transform_sender(ex::set_value_t, Sndr&& sndr, const Env&) const {
        ++then_customized;
        return std::forward<Sndr>(sndr);
    }
};

template <class S>
void dispatch(const char* name, S s) {
    then_customized   = 0;
    int         where = -99;
    auto        e     = start_env();
    std::string attrs = describe(ex::get_env(s), e);
    {
        auto op    = ex::connect(std::move(s), recorder{&where});
        int  saved = std::exchange(current_agent, 0);
        ex::start(op);
        current_agent = saved;
    }
    std::printf("%-46s attrs: %-20s D7's then chosen: %-3s ran: %s\n",
                name,
                attrs.c_str(),
                then_customized ? "yes" : "no",
                agent_name(where).c_str());
}

int main() {
    tagged<1> s1;
    tagged<7> s7;
    auto      f = [] {};
    std::puts("-- affine: elides the hop back to S0 when the child claims to be affine");
    report("affine(schedule(S1))", ex::affine(ex::schedule(s1)));
    report("affine(let_value(just(), ->schedule(S1)))",
           ex::affine(ex::let_value(ex::just(), [=] { return ex::schedule(s1); })));
    std::puts("-- dispatch: then's domain comes from its child's completion domain");
    dispatch("then(schedule(S7), f)", ex::then(ex::schedule(s7), f));
    dispatch("then(let_value(just(), ->schedule(S7)), f)",
             ex::then(ex::let_value(ex::just(), [=] { return ex::schedule(s7); }), f));
}

// Inline schedulers tagged N, each with its own (non-transforming) domain,
// and a thread_local recording which one the current code runs on, so a
// receiver can compare where it was completed with what attributes claimed.
#pragma once
#include <exec/sender_for.hpp>

#include <stdexec/execution.hpp>

#include <cstdio>
#include <string>
#include <typeinfo>
#include <utility>

namespace ex = stdexec;

inline thread_local int current_agent = -1; // -1: the main thread, no scheduler

template <int N>
struct dom {};

template <int N>
struct tagged {
    using scheduler_concept = ex::scheduler_t;
    template <class R>
    struct op {
        R    r_;
        void start() & noexcept {
            int saved = std::exchange(current_agent, N);
            ex::set_value(std::move(r_));
            current_agent = saved;
        }
    };
    struct sender {
        using sender_concept        = ex::sender_t;
        using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
        template <class R>
        auto connect(R r) const -> op<R> {
            return {std::move(r)};
        }
        struct attrs {
            auto query(ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
                return tagged{};
            }
            auto query(ex::get_completion_domain_t<ex::set_value_t>) const noexcept {
                return dom<N>{};
            }
        };
        auto get_env() const noexcept { return attrs{}; }
    };
    auto schedule() const noexcept { return sender{}; }
    auto query(ex::get_completion_domain_t<ex::set_value_t>) const noexcept {
        return dom<N>{};
    }
    auto query(ex::get_forward_progress_guarantee_t) const noexcept {
        return ex::forward_progress_guarantee::weakly_parallel;
    }
    bool operator==(const tagged&) const = default;
};

template <class T>
struct tag_of_sched {
    static std::string get() { return std::string("?") + typeid(T).name(); }
};
template <int N>
struct tag_of_sched<tagged<N>> {
    static std::string get() { return "S" + std::to_string(N); }
};
template <class T>
struct tag_of_dom {
    static std::string get() { return std::string("?") + typeid(T).name(); }
};
template <int N>
struct tag_of_dom<dom<N>> {
    static std::string get() { return "D" + std::to_string(N); }
};
template <>
struct tag_of_dom<ex::default_domain> {
    static std::string get() { return "default"; }
};

template <class Attrs, class Env>
std::string describe(const Attrs& a, const Env& e) {
    std::string out = "sched=";
    if constexpr (requires { ex::get_completion_scheduler<ex::set_value_t>(a, e); })
        out += tag_of_sched<std::remove_cvref_t<
            decltype(ex::get_completion_scheduler<ex::set_value_t>(a, e))>>::get();
    else
        out += "-";
    out += " dom=";
    if constexpr (requires { ex::get_completion_domain<ex::set_value_t>(a, e); })
        out += tag_of_dom<std::remove_cvref_t<
            decltype(ex::get_completion_domain<ex::set_value_t>(a, e))>>::get();
    else
        out += "-";
    return out;
}

// The receiver's environment: started on S0.
inline auto start_env() {
    return ex::env{ex::prop{ex::get_start_scheduler, tagged<0>{}}};
}
using start_env_t = decltype(start_env());

struct recorder {
    using receiver_concept = ex::receiver_t;
    int* where;
    template <class... As>
    void set_value(As&&...) && noexcept {
        *where = current_agent;
    }
    template <class E>
    void set_error(E&&) && noexcept {
        *where = 1000 + current_agent;
    }
    void set_stopped() && noexcept { *where = 2000 + current_agent; }
    auto get_env() const noexcept { return start_env(); }
};

inline std::string agent_name(int a) {
    std::string pre = a >= 2000 ? "stopped@" : a >= 1000 ? "error@" : "value@";
    a %= 1000;
    return pre + (a < 0 ? std::string("main") : "S" + std::to_string(a));
}

template <class S>
void report(const char* name, S s) {
    auto        e      = start_env();
    std::string before = describe(ex::get_env(s), e);
    auto&&      t      = ex::transform_sender(std::move(s), e);
    std::string after  = describe(ex::get_env(t), e);
    int         where  = -99;
    {
        // Started on S0, as the environment says.
        auto op    = ex::connect(std::forward<decltype(t)>(t), recorder{&where});
        int  saved = std::exchange(current_agent, 0);
        ex::start(op);
        current_agent = saved;
    }
    std::printf("%-44s before: %-22s after: %-22s ran: %s%s\n",
                name,
                before.c_str(),
                after.c_str(),
                agent_name(where).c_str(),
                before != after ? "   <-- attrs changed" : "");
}

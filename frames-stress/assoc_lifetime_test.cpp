#include "associate.hpp"
#include "just.hpp"
#include "repeat_n.hpp"
#include "then.hpp"
#include "when_all.hpp"

#include <cstdio>
#include <utility>
namespace ex = stdexec;
namespace ts = frames::typestate;
struct scope_t;
scope_t* g;
struct scope_t {
    bool armed     = false;
    int  live      = 0;
    bool closed    = false;
    bool refusing  = false;
    int  bad       = 0;
    int  destroyed = 0;
    class association {
      public:
        association() = default;
        explicit association(scope_t* s) noexcept : s_(s) {
            if (s_)
                ++s_->live;
        }
        association(association&& o) noexcept : s_(std::exchange(o.s_, nullptr)) {}
        auto operator=(association o) noexcept -> association& {
            std::swap(s_, o.s_);
            return *this;
        }
        ~association() {
            if (s_)
                --s_->live;
        }
        explicit operator bool() const noexcept { return s_ != nullptr; }
        auto     try_associate() const noexcept -> association {
            return association(s_ && !s_->closed ? s_ : nullptr);
        }

      private:
        scope_t* s_ = nullptr;
    };
    struct token {
        auto try_associate() const noexcept -> association {
            if (s->closed) {
                s->refusing = true;
                return association();
            }
            return association(s);
        }
        template <class S>
        auto wrap(S&& x) const noexcept -> S&& {
            return static_cast<S&&>(x);
        }
        scope_t* s;
    };
};
// A sender that may only be destroyed while associated, or while its scope refuses.
struct checked {
    using sender_concept        = ex::sender_t;
    using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
    bool tracked                = false;
    checked()                   = default;
    checked(const checked&) : tracked(g->armed) {}
    checked(checked&&) noexcept : tracked(g->armed) {}
    ~checked() {
        if (!tracked)
            return;
        ++g->destroyed;
        if (g->live == 0 && !g->refusing)
            ++g->bad;
    }
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
template <class Make>
void check(const char* name, Make make) {
    scope_t sc;
    g = &sc;
    for (bool closed : {false, true}) {
        sc.closed   = closed;
        sc.refusing = false;
        {
            checked c;
            sc.destroyed = 0;
            sc.armed     = true;
            auto s       = make(std::move(c), scope_t::token{&sc});
            if (closed && sc.destroyed < 1)
                ++sc.bad;
            sc.armed    = !closed;
            sc.refusing = false;
            ex::sync_wait(std::move(s));
        }
        sc.armed    = false;
        sc.refusing = false;
    }
    std::printf("%-26s bad %d, live at end %d\n", name, sc.bad, sc.live);
}
template <class P>
void pol(const char* name) {
    check(name, [](checked c, auto t) {
        return ts::with_policies<P>(ts::associate(std::move(c), t));
    });
    char b[64];
    std::snprintf(b, 64, "%s, repeat_n x3", name);
    check(b, [](checked c, auto t) {
        return ts::with_policies<P>(ts::repeat_n(ts::associate(std::move(c), t), 3));
    });
    std::snprintf(b, 64, "%s, when_all", name);
    check(b, [](checked c, auto t) {
        return ts::with_policies<P>(
            ts::when_all(ts::then(ts::associate(std::move(c), t), [] {})));
    });
}
int main() {
    using namespace ts;
    pol<policies<eager_teardown, carried_layout, connect_entry, stored_sender>>("eager");
    pol<policies<forward_results, carried_layout, connect_entry, stored_sender>>(
        "forward");
    pol<policies<eager_teardown, carried_layout, start_entry, stored_sender>>(
        "eager+start");
    pol<policies<eager_teardown, carried_layout, connect_entry, split_sender>>(
        "eager+split");
    pol<policies<forward_results, carried_layout, start_entry, split_sender>>(
        "fwd+start+split");
    check("stdexec",
          [](checked c, auto t) { return ex::associate(std::move(c), std::move(t)); });
}

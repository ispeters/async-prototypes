#include "erased.hpp"

#include "algos29.hpp"

#include <cstdio>
#include <cstdlib>
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)
using namespace c29e;

struct lookup_root {
    slot<value_t>            value;
    slot<std::exception_ptr> error;
    int                      how = -1;
};
struct lookup_rcvr {
    lookup_root* r;
    template <class Sig>
    void* destination() noexcept {
        if constexpr (same_as<Sig, set_value_t(value_t)>)
            return r->value.address();
        else
            return r->error.address();
    }
    template <class Sig>
    operation_handle complete() noexcept {
        r->how = erased_receiver::index<Sig>;
        if constexpr (same_as<Sig, set_value_t(value_t)>)
            r->value.adopt();
        if constexpr (same_as<Sig, set_error_t(std::exception_ptr)>)
            r->error.adopt();
        return {};
    }
};

int main() {
    {
        lookup_root                lr;
        erased_record<lookup_rcvr> rec{lookup_rcvr{&lr}};
        auto             op = c29::just(value_t("cached")).connect(erased_receiver{&rec});
        operation_handle h  = execution::start(op);
        CHECK(h.next == &rec && h.how == 0);
        execution::run(h);
        CHECK(lr.how == 0 && **lr.value == "cached");
    }
    {
        // a then behind the erasure, filtering its child's value
        lookup_root                lr;
        erased_record<lookup_rcvr> rec{lookup_rcvr{&lr}};
        auto op = c29::then(c29::just(std::string("x")), [](std::string&& s) {
                      return value_t(s + "y");
                  }).connect(erased_receiver{&rec});
        execution::run(execution::start(op));
        CHECK(lr.how == 0 && **lr.value == "xy");
    }
    std::printf("erased checks pass\n");
}

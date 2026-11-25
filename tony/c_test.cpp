#include "c_store.h"
#include "x29.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#define CHECK(x)                                                    \
    do {                                                            \
        if (!(x)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                           \
        }                                                           \
    } while (0)

struct root : async_cont {
    x29::slot<c_value> value;
    x29::slot<int>     error;
    int                how = -1;
    root();
};
constexpr x29::completion_table<2> root_table{
    [](async_cont* c, int how) noexcept -> async_next {
        auto* r = static_cast<root*>(c);
        r->how  = how;
        if (how == C_VALUE)
            r->value.adopt();
        if (how == C_ERROR)
            r->error.adopt();
        return {};
    },
    {[](async_cont* c) noexcept -> void* {
         return static_cast<root*>(c)->value.address();
     },
     [](async_cont* c) noexcept -> void* {
         return static_cast<root*>(c)->error.address();
     }}};
root::root() : async_cont{&root_table} {}

alignas(16) char stack[4096];

int main() {
    {
        root       r;
        async_next n = c_store_get_entry(c_store_default(), "a", &r, stack);
        CHECK(n.next == nullptr && r.how == -1); // pending
        CHECK(c_store_run_pending() == 1);
        CHECK(r.how == C_VALUE &&
              std::strncmp((*r.value).data, "apple", (*r.value).len) == 0);
    }
    {
        root r;
        async_run(c_store_get_entry(c_store_default(), "zz", &r, stack));
        c_store_run_pending();
        CHECK(r.how == C_VALUE && (*r.value).data == nullptr);
    }
    {
        root       r;
        async_next n = c_store_get_entry(c_store_default(), "", &r, stack);
        CHECK(n.next == &r && n.how == C_ERROR); // inline
        async_run(n);
        CHECK(r.how == C_ERROR && *r.error == EINVAL);
    }
    std::printf("C leaf checks pass\n");
}

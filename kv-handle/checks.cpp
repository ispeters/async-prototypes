// Behaviour the measurements don't exercise: a throwing factory, composition
// with a C++26 algorithm, and each compatibility pairing on the deferred path.
#include "kv_store.hpp"

#include <cstdio>
#include <stdexcept>

#define CHECK(x)                                               \
    do {                                                       \
        if (!(x)) {                                            \
            std::printf("FAILED line %d: %s\n", __LINE__, #x); \
            std::abort();                                      \
        }                                                      \
    } while (0)

struct root_cont : async_cont {
    int how = ASYNC_PENDING;
    root_cont() {
        resume = [](async_cont* c, int h) noexcept -> async_next {
            static_cast<root_cont*>(c)->how = h;
            return {nullptr, ASYNC_PENDING};
        };
    }
    void drive(async_next n) noexcept {
        if (n.next == this)
            how = n.how;
        else
            async_run(n);
        while (how == ASYNC_PENDING)
            CHECK(async_run_pending() > 0);
    }
};

struct thrower {
    hget_fn get(str key) const {
        return hget_fn(*this, std::move(key), [](const thrower&, str& key) -> hget_fn {
            throw std::runtime_error("no " + std::string(key.c_str()));
        });
    }
};

// a handle-aware receiver
static int get_handle(const kv_store_h& s, const char* key, value_t* v) {
    async_sp      sp = async_stack_create();
    slot<value_t> out;
    root_cont     k;
    {
        hget_fn fn = s.get(key);
        k.drive(fn.connect(handle_rcvr<value_t>{&out, &k, sp}).start());
    }
    if (k.how == ASYNC_VALUE)
        *v = std::move(out.value);
    out.destroy(k.how);
    async_stack_destroy(sp);
    return k.how;
}

// a C++26 receiver, below a C++26 algorithm
struct rcvr {
    using receiver_concept = ex::receiver_tag;
    int*         how;
    std::size_t* len;
    void         set_value(std::size_t n) && noexcept {
        *len = n;
        *how = ASYNC_VALUE;
    }
    void set_error(std::exception_ptr) && noexcept { *how = ASYNC_ERROR; }
    void set_stopped() && noexcept { *how = ASYNC_STOPPED; }
};

// the same, offering a strand
struct strand_rcvr : rcvr {
    async_sp*  top;
    strand_env get_env() const noexcept { return {top}; }
};

template <class Sender>
static int get_cxx26(Sender snd, std::size_t* len) {
    int  how = ASYNC_PENDING;
    auto op  = ex::connect(std::move(snd) | ex::then([](value_t v) noexcept {
                              return v ? std::string(v->c_str()).size() : std::size_t(0);
                           }),
                           rcvr{&how, len});
    ex::start(op);
    while (how == ASYNC_PENDING)
        CHECK(async_run_pending() > 0);
    return how;
}

template <class Sender>
static int get_cxx26_strand(Sender snd, std::size_t* len) {
    async_sp sp  = async_stack_create();
    int      how = ASYNC_PENDING;
    {
        auto op =
            ex::connect(std::move(snd) | ex::then([](value_t v) noexcept {
                            return v ? std::string(v->c_str()).size() : std::size_t(0);
                        }),
                        strand_rcvr{{&how, len}, &sp});
        ex::start(op);
        while (how == ASYNC_PENDING)
            CHECK(async_run_pending() > 0);
    }
    async_stack_destroy(sp);
    return how;
}

// A handle-aware sender with no C++26 `connect` of its own.
struct answer {
    using value_type = int;
    struct op {
        handle_rcvr<int> r;
        async_next       start() noexcept {
            ::new (&r.out->value) int(42);
            return {r.k, ASYNC_VALUE};
        }
    };
    op connect(handle_rcvr<int> r) const noexcept { return {r}; }
};
static_assert(handle_sender<answer, int>);
static_assert(!ex::sender<answer>); // C++26 code can't use it as it stands

// Today's `function` declared with a query connects only to receivers that
// answer the query. Every receiver answers one that has a default.
struct value_rcvr {
    using receiver_concept = ex::receiver_tag;
    void set_value(value_t) && noexcept {}
    void set_error(std::exception_ptr) && noexcept {}
    void set_stopped() && noexcept {}
};
struct value_strand_rcvr : value_rcvr {
    async_sp*  top;
    strand_env get_env() const noexcept { return {top}; }
};
static_assert(ex::sender_to<get_fn, value_rcvr>);
static_assert(ex::sender_to<get_fn_cursor, value_strand_rcvr>);
static_assert(ex::sender_to<get_fn_cursor, value_rcvr>); // the query has a default
static_assert(ex::sender_to<get_fn_alloc, value_strand_rcvr>);
static_assert(!ex::sender_to<get_fn_alloc, value_rcvr>);

int main() {
    // every pairing, inline and deferred, hit-after-miss and absent key
    static const char pairs[][2] = {{'n', 'n'},
                                    {'o', 'o'},
                                    {'w', 'w'},
                                    {'n', 'p'},
                                    {'n', 'l'},
                                    {'n', 'w'},
                                    {'w', 'n'},
                                    {'o', 'n'},
                                    {'p', 'n'},
                                    {'p', 'w'},
                                    {'a', 'a'},
                                    {'a', 'n'},
                                    {'c', 'c'},
                                    {'c', 'n'}};
    for (const auto& p : pairs)
        for (bool deferred : {false, true}) {
            stores s = make_stores(p[0], p[1], true, deferred);
            for (const char* key : {"k1", "k1", "kx"}) {
                bool present = key[1] == '1';
                if (s.hidden) {
                    value_t v;
                    CHECK(get_handle(*s.hidden, key, &v) == ASYNC_VALUE);
                    CHECK(bool(v) == present);
                    std::size_t len = 99;
                    CHECK(get_cxx26(s.hidden->get(key), &len) == ASYNC_VALUE);
                    CHECK(len == (present ? 2u : 0u));
                } else if (s.today) {
                    std::size_t len = 99;
                    CHECK(get_cxx26(s.today->get(key), &len) == ASYNC_VALUE);
                    CHECK(len == (present ? 2u : 0u));
                } else {
                    // a `function` that forwards the strand: no stack is rooted below it
                    std::size_t len   = 99;
                    std::size_t slabs = async_slabs_allocated;
                    int how = s.alloc ? get_cxx26_strand(s.alloc->get(key), &len)
                                      : get_cxx26_strand(s.cursor->get(key), &len);
                    CHECK(how == ASYNC_VALUE);
                    CHECK(len == (present ? 2u : 0u));
                    CHECK(async_slabs_allocated == slabs + 1); // the root's own
                }
            }
        }

    // a throwing factory is an error completion, for both kinds of receiver
    {
        thrower       t;
        async_sp      sp = async_stack_create();
        slot<value_t> out;
        root_cont     k;
        hget_fn       fn = t.get("k1");
        k.drive(fn.connect(handle_rcvr<value_t>{&out, &k, sp}).start());
        CHECK(k.how == ASYNC_ERROR);
        try {
            std::rethrow_exception(out.error);
        } catch (const std::runtime_error& e) {
            CHECK(std::string(e.what()) == "no k1");
        }
        out.destroy(k.how);
        async_stack_destroy(sp);
        std::size_t len;
        CHECK(get_cxx26(t.get("k1"), &len) == ASYNC_ERROR);
    }
    // the generic operation-state adaptor, around a sender that isn't `hfn`
    {
        struct int_rcvr {
            using receiver_concept = ex::receiver_tag;
            int* v;
            void set_value(int n) && noexcept { *v = n; }
            void set_error(std::exception_ptr) && noexcept { *v = -1; }
            void set_stopped() && noexcept { *v = -2; }
        };
        struct int_strand_rcvr : int_rcvr {
            async_sp*  top;
            strand_env get_env() const noexcept { return {top}; }
        };
        auto plus_one = ex::then([](int n) noexcept { return n + 1; });

        int         v     = 0;
        std::size_t slabs = async_slabs_allocated;
        {
            auto op = ex::connect(as_cxx26(answer{}) | plus_one, int_rcvr{&v});
            ex::start(op);
        }
        CHECK(v == 43);
        CHECK(async_slabs_allocated == slabs + 1); // nothing offered: it roots a stack

        async_sp sp = async_stack_create();
        v           = 0;
        slabs       = async_slabs_allocated;
        {
            auto op =
                ex::connect(as_cxx26(answer{}) | plus_one, int_strand_rcvr{{&v}, &sp});
            ex::start(op);
        }
        CHECK(v == 43);
        CHECK(async_slabs_allocated == slabs); // the cursor came through `then`
        async_stack_destroy(sp);
    }
    std::puts("checks passed");
}

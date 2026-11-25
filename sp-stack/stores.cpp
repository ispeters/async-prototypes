#include "stores.hpp"

// The two bodies, each written three times:
//
//   async value_t mem_store::get(str key) const {
//     co_await maybe_defer{deferred};
//     co_return find(key);
//   }
//
//   async value_t cache::get(str key) const {
//     if (auto hit = entries_.find(key))
//       co_return hit;
//     auto value = co_await backing_->get(key);
//     if (value)
//       entries_.insert(key, *value);
//     co_return value;
//   }

namespace {

struct mem_data {
    std::map<str, str> data;
    bool               deferred;
    value_t            find(const str& key) const {
        if (auto it = data.find(key); it != data.end())
            return it->second;
        return std::nullopt;
    }
};

// ============================================================================
// SYNCHRONOUS
// ============================================================================
struct mem_s final : kv_store, mem_data {
    mem_s(mem_data d) : mem_data(std::move(d)) {}
    value_t get_sync(str key) const override { return find(key); }
};

struct cache_s final : kv_store {
    const kv_store*       backing_;
    mutable cache_entries entries_;
    cache_s(const kv_store& b, bool retain) : backing_(&b), entries_(retain) {}
    value_t get_sync(str key) const override {
        if (auto hit = entries_.find(key))
            return hit;
        auto value = backing_->get_sync(key);
        if (value)
            entries_.insert(key, *value);
        return value;
    }
};

// ============================================================================
// CURSOR: kv-handle's visible entry (stores.cpp there, `mem_body`, `cache_body`
// and `await_get<kv_store_v>`), with the templates removed
// ============================================================================
struct mem_frame_v : async_task {
    const mem_data* self;
    str*            key;
    slot<value_t>*  out;
    async_cont*     k;
};

struct mem_v final : kv_store_v, mem_data {
    mem_v(mem_data d) : mem_data(std::move(d)) {}
    async_next get_entry(str&           key,
                         slot<value_t>* out,
                         async_cont*    k,
                         async_sp       sp) const noexcept override {
        if (!deferred) {
            ::new (&out->value) value_t(find(key));
            return {k, ASYNC_VALUE};
        }
        async_sp top = sp;
        auto*    f   = ::new (async_push(&top, sizeof(mem_frame_v))) mem_frame_v;
        f->self      = this;
        f->key       = &key;
        f->out       = out;
        f->k         = k;
        f->run       = [](async_task* t) noexcept -> async_next {
            auto* f = static_cast<mem_frame_v*>(t);
            ::new (&f->out->value) value_t(f->self->find(*f->key));
            async_cont* up = f->k;
            async_sp    top;
            async_pop(&top, f);
            return {up, ASYNC_VALUE};
        };
        async_defer(f);
        return {nullptr, ASYNC_PENDING};
    }
};

struct cache_frame_v : async_cont {
    cache_entries* entries;
    str*           key;
    slot<value_t>* out;
    async_cont*    k;
    // the callee's parameter object
    union {
        str t;
    };
    cache_frame_v() noexcept {}
    ~cache_frame_v() {}

    async_next after_await(int how) noexcept {
        t.~str();
        if (how == ASYNC_VALUE) {
            value_t& v = out->value;
            if (v)
                entries->insert(*key, *v);
        }
        async_cont* up = k;
        async_sp    top;
        async_pop(&top, this);
        return {up, how};
    }
};

struct cache_v final : kv_store_v {
    const kv_store_v*     backing_;
    mutable cache_entries entries_;
    cache_v(const kv_store_v& b, bool retain) : backing_(&b), entries_(retain) {}
    async_next get_entry(str&           key,
                         slot<value_t>* out,
                         async_cont*    k,
                         async_sp       sp) const noexcept override {
        value_t& hit = *::new (&out->value) value_t(entries_.find(key));
        if (hit)
            return {k, ASYNC_VALUE};
        hit.~value_t();
        async_sp top = sp;
        auto*    f   = ::new (async_push(&top, sizeof(cache_frame_v))) cache_frame_v;
        f->entries   = &entries_;
        f->key       = &key;
        f->out       = out;
        f->k         = k;
        f->resume    = [](async_cont* c, int how) noexcept {
            return static_cast<cache_frame_v*>(c)->after_await(how);
        };
        ::new (&f->t) str(key);
        async_next n = backing_->get_entry(f->t, out, f, top);
        if (n.next == f)
            return f->after_await(n.how);
        return n;
    }
};

// ============================================================================
// MACHINE STACK: the same two bodies. The frame is a local variable; a
// suspending function returns through MS_LEAVE, which leaves it in place.
// ============================================================================
struct mem_frame_m : ms::cont {
    const mem_data* self;
    str*            key;
    slot<value_t>*  out;
    ms::cont*       k;
};

struct mem_m final : kv_store_m, mem_data {
    mem_m(mem_data d) : mem_data(std::move(d)) {}

    static ms::next finish(ms::cont* c, int) noexcept {
        auto* f = static_cast<mem_frame_m*>(c);
        ::new (&f->out->value) value_t(f->self->find(*f->key));
        return {
            f->k,
            ASYNC_VALUE}; // whoever runs k pops this frame by moving the stack pointer
    }

    [[gnu::noinline]] ms::next
    get_entry(str& key, slot<value_t>* out, ms::cont* k) const noexcept override {
        if (!deferred) {
            ::new (&out->value) value_t(find(key));
            return {k, ASYNC_VALUE};
        }
        mem_frame_m f;
        f.resume = &finish;
        f.sp     = MS_SP();
        f.self   = this;
        f.key    = &key;
        f.out    = out;
        f.k      = k;
        ms::defer(&f);
        MS_LEAVE((ms::next{nullptr, ASYNC_PENDING}));
    }
};

struct cache_frame_m : ms::cont {
    cache_entries* entries;
    str*           key;
    slot<value_t>* out;
    ms::cont*      k;
    union {
        str t;
    };
    cache_frame_m() noexcept {}
    ~cache_frame_m() {}

    ms::next after_await(int how) noexcept {
        t.~str();
        if (how == ASYNC_VALUE) {
            value_t& v = out->value;
            if (v)
                entries->insert(*key, *v);
        }
        return {k, how};
    }
};

struct cache_m final : kv_store_m {
    const kv_store_m*     backing_;
    mutable cache_entries entries_;
    cache_m(const kv_store_m& b, bool retain) : backing_(&b), entries_(retain) {}

    static ms::next resumed(ms::cont* c, int how) noexcept {
        return static_cast<cache_frame_m*>(c)->after_await(how);
    }

    [[gnu::noinline]] ms::next
    get_entry(str& key, slot<value_t>* out, ms::cont* k) const noexcept override {
        value_t& hit = *::new (&out->value) value_t(entries_.find(key));
        if (hit)
            return {k, ASYNC_VALUE};
        hit.~value_t();
        cache_frame_m f;
        f.resume  = &resumed;
        f.sp      = MS_SP();
        f.entries = &entries_;
        f.key     = &key;
        f.out     = out;
        f.k       = k;
        ::new (&f.t) str(key);
        ms::next n = backing_->get_entry(f.t, out, &f);
        MS_CLOBBER();
        if (n.k != &f) { // the callee suspended, and its frame is below ours
            if (ms::after_pending)
                ms::after_pending();
            MS_LEAVE(n);
        }
        return f.after_await(n.how);
    }
};

} // namespace

stores make_stores(bool retain, bool deferred) {
    mem_data data{{{"k1", "v1"}, {"k2", "v2"}}, deferred};
    return {new cache_s(*new mem_s(data), retain),
            new cache_v(*new mem_v(data), retain),
            new cache_m(*new mem_m(data), retain)};
}

// ============================================================================
// The caller that awaits in a loop
// ============================================================================
std::size_t count_found_sync(const kv_store& s, const str& key, long n) {
    std::size_t total = 0;
    for (long i = 0; i < n; ++i)
        if (s.get_sync(key))
            ++total;
    return total;
}

namespace {

template <class Cont, class Store>
struct count_frame : Cont {
    const Store*       s;
    const str*         key;
    long               i, n;
    std::size_t        total;
    slot<std::size_t>* out;
    Cont*              k;
    // the callee's parameter object, for one iteration
    union {
        str arg;
    };
    // the await's result, for one iteration
    slot<value_t> v;
    count_frame() noexcept {}
    ~count_frame() {}

    void step(int how) noexcept {
        arg.~str();
        if (how == ASYNC_VALUE && v.value)
            ++total;
        v.destroy(how);
        ++i;
    }
};

struct count_frame_c : count_frame<async_cont, kv_store_v> {
    async_sp   top;
    async_next loop() noexcept {
        while (i < n) {
            ::new (&arg) str(*key);
            async_next r = s->get_entry(arg, &v, this, top);
            if (r.next != this)
                return r;
            step(r.how);
        }
        ::new (&out->value) std::size_t(total);
        async_cont* up = k;
        async_sp    t;
        async_pop(&t, this);
        return {up, ASYNC_VALUE};
    }
};

struct count_frame_m : count_frame<ms::cont, kv_store_m> {
    // Inlined into the entry and into `resumed`, so that MS_LEAVE leaves the
    // frame of whichever is running.
    [[gnu::always_inline]] ms::next loop() noexcept {
        while (i < n) {
            ::new (&arg) str(*key);
            ms::next r = s->get_entry(arg, &v, this);
            MS_CLOBBER();
            if (r.k != this)
                MS_LEAVE(r);
            step(r.how);
        }
        ::new (&out->value) std::size_t(total);
        return {k, ASYNC_VALUE};
    }
    [[gnu::noinline]] static ms::next resumed(ms::cont* c, int how) noexcept {
        auto* f = static_cast<count_frame_m*>(c);
        f->step(how);
        return f->loop();
    }
};

} // namespace

async_next count_found_c(const kv_store_v&  s,
                         const str&         key,
                         long               n,
                         slot<std::size_t>* out,
                         async_cont*        k,
                         async_sp           sp) noexcept {
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(count_frame_c))) count_frame_c;
    f->s         = &s;
    f->key       = &key;
    f->i         = 0;
    f->n         = n;
    f->total     = 0;
    f->out       = out;
    f->k         = k;
    f->top       = top;
    f->resume    = [](async_cont* c, int how) noexcept {
        auto* f = static_cast<count_frame_c*>(c);
        f->step(how);
        return f->loop();
    };
    return f->loop();
}

std::size_t machine_frame_bytes[3] = {
    sizeof(mem_frame_m), sizeof(cache_frame_m), sizeof(count_frame_m)};
std::size_t cursor_frame_bytes[3] = {
    sizeof(mem_frame_v), sizeof(cache_frame_v), sizeof(count_frame_c)};

[[gnu::noinline]] ms::next count_found_m(const kv_store_m&  s,
                                         const str&         key,
                                         long               n,
                                         slot<std::size_t>* out,
                                         ms::cont*          k) noexcept {
    count_frame_m f;
    f.resume = &count_frame_m::resumed;
    f.sp     = MS_SP();
    f.s      = &s;
    f.key    = &key;
    f.i      = 0;
    f.n      = n;
    f.total  = 0;
    f.out    = out;
    f.k      = k;
    return f.loop();
}

// ============================================================================
// The caller that forks
// ============================================================================
namespace {

template <class Cont, class Parent>
struct child : Cont {
    Parent* parent;
    int     how;
    union {
        str arg;
    };
    slot<value_t> v;
    child() noexcept {}
    ~child() {}
    bool finish() noexcept { // returns whether a value was found
        arg.~str();
        bool found = how == ASYNC_VALUE && v.value;
        v.destroy(how);
        return found;
    }
};

struct all_frame_m;
struct child_m : child<ms::cont, all_frame_m> {
    ms::stack stk;
};

struct all_frame_m : ms::cont {
    ms::cont*           k;
    slot<std::size_t>*  out;
    ms::stack_provider* stacks;
    child_m*            kids;
    std::size_t         n, remaining;

    ms::next finish() noexcept {
        std::size_t found = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (kids[i].finish())
                ++found;
            if (kids[i].stk.base)
                stacks->release(kids[i].stk);
        }
        ::new (&out->value) std::size_t(found);
        return {k, ASYNC_VALUE};
    }
    static ms::next joined(ms::cont* c, int) noexcept {
        return static_cast<all_frame_m*>(c)->finish();
    }
    // Runs on the child's stack, which is empty by then. Handing back the
    // parent moves the stack pointer to the parent's stack.
    static ms::next child_done(ms::cont* c, int how) noexcept {
        auto* kid      = static_cast<child_m*>(c);
        kid->how       = how;
        all_frame_m* p = kid->parent;
        if (--p->remaining == 0)
            return {p, ASYNC_VALUE};
        return {nullptr, ASYNC_PENDING};
    }
};

struct all_frame_c;
struct child_c : child<async_cont, all_frame_c> {
    async_sp root;
};

struct all_frame_c : async_cont {
    async_cont*        k;
    slot<std::size_t>* out;
    child_c*           kids;
    std::size_t        n, remaining;

    async_next finish() noexcept {
        std::size_t found = 0;
        for (std::size_t i = 0; i < n; ++i) {
            if (kids[i].finish())
                ++found;
            if (kids[i].root)
                async_stack_destroy(kids[i].root);
        }
        std::free(kids);
        ::new (&out->value) std::size_t(found);
        async_cont* up = k;
        async_sp    t;
        async_pop(&t, this);
        return {up, ASYNC_VALUE};
    }
};

} // namespace

[[gnu::noinline]] ms::next count_all_m(const kv_store_m&   s,
                                       const str*          keys,
                                       std::size_t         n,
                                       ms::stack_provider& stacks,
                                       slot<std::size_t>*  out,
                                       ms::cont*           k) noexcept {
    all_frame_m f;
    f.resume    = &all_frame_m::joined;
    f.k         = k;
    f.out       = out;
    f.stacks    = &stacks;
    f.n         = n;
    f.remaining = 0;
    // The children's records are part of this frame: a stack-pointer bump.
    f.kids = static_cast<child_m*>(__builtin_alloca(n * sizeof(child_m)));
    f.sp   = MS_SP();
    for (std::size_t i = 0; i < n; ++i) {
        child_m* kid = ::new (&f.kids[i]) child_m;
        kid->parent  = &f;
        kid->resume  = &all_frame_m::child_done;
        ::new (&kid->arg) str(keys[i]);
        kid->stk = stacks.acquire();
        kid->sp  = kid->stk.top();
        ms::next r =
            ms::enter(kid->stk.top(), kid, [&] [[gnu::always_inline]] () noexcept {
                return s.get_entry(kid->arg, &kid->v, kid);
            });
        if (r.k == kid) { // completed inline: its stack is free again
            kid->how = r.how;
            stacks.release(kid->stk);
            kid->stk = {};
        } else if (!r.k) {
            ++f.remaining;
        } else {
            std::abort();
        }
    }
    if (f.remaining == 0)
        return f.finish();
    MS_LEAVE((ms::next{nullptr, ASYNC_PENDING}));
}

async_next count_all_c(const kv_store_v&  s,
                       const str*         keys,
                       std::size_t        n,
                       slot<std::size_t>* out,
                       async_cont*        k,
                       async_sp           sp) noexcept {
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(all_frame_c))) all_frame_c;
    f->resume    = [](async_cont* c, int) noexcept {
        return static_cast<all_frame_c*>(c)->finish();
    };
    f->k         = k;
    f->out       = out;
    f->n         = n;
    f->remaining = 0;
    f->kids      = static_cast<child_c*>(std::malloc(n * sizeof(child_c)));
    if (!f->kids)
        std::abort();
    for (std::size_t i = 0; i < n; ++i) {
        child_c* kid = ::new (&f->kids[i]) child_c;
        kid->parent  = f;
        kid->resume  = [](async_cont* c, int how) noexcept -> async_next {
            auto* kid      = static_cast<child_c*>(c);
            kid->how       = how;
            all_frame_c* p = kid->parent;
            if (--p->remaining == 0)
                return {p, ASYNC_VALUE};
            return {nullptr, ASYNC_PENDING};
        };
        ::new (&kid->arg) str(keys[i]);
        kid->root    = async_stack_create();
        async_next r = s.get_entry(kid->arg, &kid->v, kid, kid->root);
        if (r.next == kid) {
            kid->how = r.how;
            async_stack_destroy(kid->root);
            kid->root = nullptr;
        } else if (!r.next) {
            ++f->remaining;
        } else {
            std::abort();
        }
    }
    if (f->remaining == 0)
        return f->finish();
    return {nullptr, ASYNC_PENDING};
}

#include "tree.hpp"

bool walk_leaves_suspend;

[[gnu::noinline]] long walk_sync(const node* t) {
    if (!t)
        return 0;
    long a = walk_sync(t->left);
    long b = walk_sync(t->right);
    return (a ^ (b << 1)) + t->value;
}

namespace {

// ---- cursor ------------------------------------------------------------------
struct leaf_c : async_task {
    long*       out;
    async_cont* k;
};

struct walk_frame_c : async_cont {
    const node* t;
    long*       out;
    async_cont* k;
    async_sp    top;
    long        a, b;
    int         state;

    async_next finish() noexcept {
        *out           = (a ^ (b << 1)) + t->value;
        async_cont* up = k;
        async_sp    s;
        async_pop(&s, this);
        return {up, ASYNC_VALUE};
    }
    static async_next resumed(async_cont* c, int) noexcept {
        auto* f = static_cast<walk_frame_c*>(c);
        if (f->state == 1) {
            f->state     = 2;
            async_next n = walk_c(f->t->right, &f->b, f, f->top);
            if (n.next != f)
                return n;
        }
        return f->finish();
    }
};

// ---- machine stack -----------------------------------------------------------
struct leaf_m : ms::cont {
    long*           out;
    ms::cont*       k;
    static ms::next run(ms::cont* c, int) noexcept {
        auto* f = static_cast<leaf_m*>(c);
        *f->out = 0;
        return {f->k, ASYNC_VALUE};
    }
};

struct walk_frame_m : ms::cont {
    const node* t;
    long*       out;
    ms::cont*   k;
    long        a, b;
    int         state;

    [[gnu::noinline]] static ms::next resumed(ms::cont* c, int) noexcept {
        auto* f = static_cast<walk_frame_m*>(c);
        if (f->state == 1) {
            f->state   = 2;
            ms::next n = walk_m(f->t->right, &f->b, f);
            MS_CLOBBER();
            if (n.k != f)
                MS_LEAVE(n);
        }
        *f->out = (f->a ^ (f->b << 1)) + f->t->value;
        return {f->k, ASYNC_VALUE};
    }
};

} // namespace

std::size_t walk_frame_bytes[2] = {sizeof(walk_frame_c), sizeof(walk_frame_m)};

[[gnu::noinline]] async_next
walk_c(const node* t, long* out, async_cont* k, async_sp sp) noexcept {
    if (!t) {
        if (!walk_leaves_suspend) {
            *out = 0;
            return {k, ASYNC_VALUE};
        }
        async_sp top = sp;
        auto*    f   = ::new (async_push(&top, sizeof(leaf_c))) leaf_c;
        f->out       = out;
        f->k         = k;
        f->run       = [](async_task* t) noexcept -> async_next {
            auto* f        = static_cast<leaf_c*>(t);
            *f->out        = 0;
            async_cont* up = f->k;
            async_sp    s;
            async_pop(&s, f);
            return {up, ASYNC_VALUE};
        };
        async_defer(f);
        return {nullptr, ASYNC_PENDING};
    }
    async_sp top = sp;
    auto*    f   = ::new (async_push(&top, sizeof(walk_frame_c))) walk_frame_c;
    f->resume    = &walk_frame_c::resumed;
    f->t         = t;
    f->out       = out;
    f->k         = k;
    f->top       = top;
    f->state     = 1;
    async_next n = walk_c(t->left, &f->a, f, top);
    if (n.next != f)
        return n;
    f->state = 2;
    n        = walk_c(t->right, &f->b, f, top);
    if (n.next != f)
        return n;
    return f->finish();
}

[[gnu::noinline]] ms::next walk_m(const node* t, long* out, ms::cont* k) noexcept {
    if (!t) {
        if (!walk_leaves_suspend) {
            *out = 0;
            return {k, ASYNC_VALUE};
        }
        leaf_m f;
        f.resume = &leaf_m::run;
        f.sp     = MS_SP();
        f.out    = out;
        f.k      = k;
        ms::defer(&f);
        MS_LEAVE((ms::next{nullptr, ASYNC_PENDING}));
    }
    walk_frame_m f;
    f.resume   = &walk_frame_m::resumed;
    f.sp       = MS_SP();
    f.t        = t;
    f.out      = out;
    f.k        = k;
    f.state    = 1;
    ms::next n = walk_m(t->left, &f.a, &f);
    MS_CLOBBER();
    if (n.k != &f)
        MS_LEAVE(n);
    f.state = 2;
    n       = walk_m(t->right, &f.b, &f);
    MS_CLOBBER();
    if (n.k != &f)
        MS_LEAVE(n);
    *out = (f.a ^ (f.b << 1)) + t->value;
    return {k, ASYNC_VALUE};
}

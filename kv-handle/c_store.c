/* The implementation side, in C11. No C++ runtime is involved. */
#include "c_store.h"

#include <errno.h>
#include <string.h>

struct c_store {
    const char* keys[2];
    const char* values[2];
};

c_store* c_store_open(void) {
    static c_store s = {{"k1", "k2"}, {"v1", "value two"}};
    return &s;
}

static async_next nothing_ready(void) {
    async_next n = {NULL, ASYNC_PENDING};
    return n;
}

static async_next completed(async_cont* k, int how) {
    async_next n = {k, how}; /* completing is handing back the caller's record */
    return n;
}

/* ---- c_store_get: a leaf that suspends ---------------------------------- */

typedef struct get_frame {
    async_task     task; /* first member, so a task pointer is a frame pointer */
    const c_store* store;
    const char*    key;
    c_get_slot*    out;
    async_cont*    k;
} get_frame;

static async_next get_run(async_task* t) {
    get_frame*  f   = (get_frame*)t;
    c_get_slot* out = f->out;
    async_cont* k   = f->k;
    out->value.data = NULL;
    out->value.len  = 0;
    for (int i = 0; i < 2; ++i) {
        if (strcmp(f->store->keys[i], f->key) == 0) {
            out->value.data = f->store->values[i];
            out->value.len  = strlen(f->store->values[i]);
        }
    }
    async_sp top;
    async_pop(&top, f);               /* the frame is gone before the caller continues */
    return completed(k, ASYNC_VALUE); /* the event loop runs the caller */
}

async_next c_store_get_entry(
    const c_store* store, const char* key, c_get_slot* out, async_cont* k, async_sp sp) {
    if (key[0] == '\0') { /* fails inline, with no frame */
        out->error = EINVAL;
        return completed(k, ASYNC_ERROR);
    }
    async_sp   top = sp;
    get_frame* f   = (get_frame*)async_push(&top, sizeof(get_frame));
    f->task.run    = get_run;
    f->store       = store;
    f->key         = key; /* borrowed: the caller keeps it valid until we complete */
    f->out         = out;
    f->k           = k;
    async_defer(&f->task);
    return nothing_ready();
}

/* ---- c_store_get_len: the hand-lowered form of ----------------------------
 *
 *   c_len_fn c_store_get_len(c_store const* store, char const* key) {
 *     c_value v = co_await c_store_get(store, key);
 *     co_return v.len;
 *   }
 */

typedef struct len_frame {
    async_cont  cont; /* the continuation record our callee hands back */
    c_len_slot* out;
    async_cont* k;
    c_get_slot  v; /* the local `v`: the callee's result slot */
} len_frame;

/* The rest of the body after the await. It exists once: it's reached by
 * falling through, or as the frame's resume function. */
static async_next len_after_await(async_cont* c, int how) {
    len_frame*  f = (len_frame*)c;
    async_cont* k = f->k;
    if (how == ASYNC_VALUE)
        f->out->value = f->v.value.len;
    else if (how == ASYNC_ERROR)
        f->out->error = f->v.error; /* forwarded */
    async_sp top;
    async_pop(&top, f);
    return completed(k, how);
}

async_next c_store_get_len_entry(
    const c_store* store, const char* key, c_len_slot* out, async_cont* k, async_sp sp) {
    async_sp   top = sp;
    len_frame* f   = (len_frame*)async_push(&top, sizeof(len_frame));
    f->cont.resume = len_after_await;
    f->out         = out;
    f->k           = k;
    async_next n   = c_store_get_entry(store, key, &f->v, &f->cont, top);
    if (n.next == &f->cont)
        return len_after_await(&f->cont, n.how); /* handed our own record */
    return n;
}

/* The asynchronous calling convention used by this experiment, in C.
 *
 * An asynchronous function is reached through its *entry*:
 *
 *     async_next entry(args..., slot* out, async_cont* k, async_sp sp);
 *
 * - args: passed as the platform's C or C++ ABI passes them. What the caller
 *   passes by reference (including by-value class types, which the Itanium
 *   ABI passes by invisible reference) stays valid until the operation
 *   *completes*, not just until the entry returns.
 * - out: uninitialized storage in the caller, big enough for any declared
 *   completion's payload. The callee constructs the result there.
 * - k: the continuation record, which lives in the caller's frame.
 * - sp: the top of the caller's strand of the async stack. The callee pushes
 *   its frame there and pops it before it completes.
 *
 * An entry, and a continuation's resume function, return the continuation
 * that is ready to run next and how the operation it was waiting for
 * completed, or {NULL} if nothing is ready. To complete, an operation returns
 * its caller's record; it never calls it. Whoever is at the bottom of the
 * machine stack runs what comes back (async_run). A caller that is handed its
 * own record may carry on in place.
 *
 * In sender terms: the returned value is what `start` returns; `out`, `k` and
 * `sp` are what the receiver offers.
 */
#ifndef ASYNC_ABI_H
#define ASYNC_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
    #define ASYNC_NOEXCEPT noexcept
#else
    #define ASYNC_NOEXCEPT
#endif

/* Completions. Non-negative values index the function's declared
 * completions; every function in this experiment declares these three. */
enum { ASYNC_PENDING = -1, ASYNC_VALUE = 0, ASYNC_ERROR = 1, ASYNC_STOPPED = 2 };

typedef struct async_cont async_cont;
typedef struct async_next {
    async_cont* next;
    int         how;
} async_next; /* returned in two registers */

struct async_cont {
    async_next (*resume)(async_cont* self, int how) ASYNC_NOEXCEPT;
    /* The caller's environment (stop state, scheduler, ...) would be reached
     * from here. Not prototyped. */
};

inline static void async_run(async_next n) {
    while (n.next) {
        n = n.next->resume(n.next, n.how);
    }
}

/* The async stack: a chain of aligned slabs. A cursor is one pointer; the
 * slab's header is found by masking. */
typedef char* async_sp;

enum { ASYNC_SLAB_SIZE = 4096 };

typedef struct async_slab {
    struct async_slab* prev;     /* slab below this one, or NULL */
    async_sp           prev_top; /* where the strand stood when this slab was added */
} async_slab;

extern size_t async_high_water; /* statistics only */
extern size_t async_slabs_allocated;

inline static async_slab* async_slab_of(async_sp sp) {
    return (async_slab*)((uintptr_t)(sp - 1) & ~(uintptr_t)(ASYNC_SLAB_SIZE - 1));
}

inline static async_sp async_slab_new(async_slab* prev, async_sp prev_top) {
    async_slab* s = (async_slab*)aligned_alloc(ASYNC_SLAB_SIZE, ASYNC_SLAB_SIZE);
    if (!s) {
        abort();
    }
    s->prev     = prev;
    s->prev_top = prev_top;
    ++async_slabs_allocated;
    return (async_sp)s + sizeof(async_slab);
}

inline static async_sp async_stack_create(void) { return async_slab_new(NULL, NULL); }
inline static void     async_stack_destroy(async_sp sp) { free(async_slab_of(sp)); }

/* Pushes n bytes (16-byte aligned) at *top and advances it. */
inline static void* async_push(async_sp* top, size_t n) {
    async_sp    sp   = *top;
    async_slab* slab = async_slab_of(sp);
    n                = (n + 15) & ~(size_t)15;
    if (__builtin_expect((size_t)(sp - (async_sp)slab) + n > ASYNC_SLAB_SIZE, 0)) {
        if (n > ASYNC_SLAB_SIZE - sizeof(async_slab)) {
            abort(); /* prototype limit */
        }
        sp = async_slab_new(slab, sp);
    }
    *top = sp + n;
#ifdef ASYNC_STATS
    {
        size_t used = (size_t)(*top - (async_sp)async_slab_of(*top));
        if (used > async_high_water) {
            async_high_water = used;
        }
    }
#endif
    return sp;
}

/* Pops the frame pushed most recently on this strand; *top returns to where
 * it was before the push. */
inline static void async_pop(async_sp* top, void* frame) {
    async_slab* slab = async_slab_of((async_sp)frame + 1);
    if (__builtin_expect((async_sp)frame == (async_sp)slab + sizeof(async_slab) &&
                             slab->prev != NULL,
                         0)) {
        *top = slab->prev_top;
        free(slab);
    } else {
        *top = (async_sp)frame;
    }
}

/* A queue of deferred work, standing in for an event loop. The loop is the
 * bottom of the machine stack: it runs whatever a task hands back. */
typedef struct async_task {
    async_next (*run)(struct async_task* self) ASYNC_NOEXCEPT;
    struct async_task* next;
} async_task;
void async_defer(async_task* t) ASYNC_NOEXCEPT;
int  async_run_pending(void) ASYNC_NOEXCEPT; /* returns the number of tasks run */

#ifdef __cplusplus
}
#endif
#endif

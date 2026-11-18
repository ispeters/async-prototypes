/* The erased-call ABI the C++29 column assumes (4A), in C.
 *
 * An asynchronous function's entry is
 *
 *     async_next entry(args..., async_cont* k, async_sp sp);
 *
 * k is the caller's continuation record: one pointer to a static table whose
 * first member is the resume function, followed by one destination function
 * per payload-carrying completion, in the order the function declares them.
 * sp is the top of the caller's strand of the async stack.
 *
 * Completing never calls the caller: it returns {k, how}. Whoever is at the
 * bottom of the machine stack runs what comes back (async_run); a caller that
 * gets its own record back may carry on in place.
 *
 * Built from kv-dest/async_abi.h; the async stack here is a bump pointer into
 * one block, which is all these examples need.
 */
#ifndef ABI4A_H
#define ABI4A_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#define ASYNC_NOEXCEPT noexcept
#else
#define ASYNC_NOEXCEPT
#endif

enum { ASYNC_PENDING = -1 };

typedef struct async_cont async_cont;
typedef struct async_next {
  async_cont* next; /* NULL: nothing is ready */
  int how;          /* which declared completion happened */
} async_next;       /* returned in two registers */

typedef async_next (*async_resume_fn)(async_cont* k, int how) ASYNC_NOEXCEPT;
typedef void* (*async_dest_fn)(async_cont* k) ASYNC_NOEXCEPT;

/* A table for a function with N payload-carrying completions. */
#define ASYNC_TABLE(N)              \
  struct {                          \
    async_resume_fn resume;         \
    async_dest_fn destination[N];   \
  }

struct async_cont {
  void const* table; /* an ASYNC_TABLE(N) */
};

static inline async_resume_fn async_resume_of(async_cont* k) {
  return *(async_resume_fn const*)k->table;
}
static inline async_dest_fn async_destination_of(async_cont* k, int i) {
  return ((async_dest_fn const*)((char const*)k->table + sizeof(async_resume_fn)))[i];
}

static inline async_next async_complete(async_cont* k, int how) {
  async_next n = {k, how};
  return n;
}

static inline void async_run(async_next n) {
  while (n.next) n = async_resume_of(n.next)(n.next, n.how);
}

/* The async stack: here, a bump pointer. */
typedef char* async_sp;
static inline void* async_push(async_sp* top, size_t n) {
  void* p = *top;
  *top += (n + 15) & ~(size_t)15;
  return p;
}

#ifdef __cplusplus
}
#endif
#endif

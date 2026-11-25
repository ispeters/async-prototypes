/* An asynchronous interface whose types are C types, so that either side of
 * it can be written in C. The C++ declarations these entries stand behind:
 *
 *   using c_get_fn = exec::function<
 *       ex::sender_tag(c_store const*, char const*),
 *       ex::completion_signatures<ex::set_value_t(c_value),
 *                                 ex::set_error_t(int),
 *                                 ex::set_stopped_t()>>;
 *   c_get_fn c_store_get(c_store const* store, char const* key);
 *
 * and likewise c_store_get_len, completing with set_value_t(size_t).
 */
#ifndef C_STORE_H
#define C_STORE_H
#include "async_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c_store c_store;
c_store*               c_store_open(void);

typedef struct c_value {
    const char* data; /* NULL when the key is absent */
    size_t      len;
} c_value;

/* One member per declared completion that has a payload. */
typedef union c_get_slot {
    c_value value;
    int     error;
} c_get_slot;

typedef union c_len_slot {
    size_t value;
    int    error;
} c_len_slot;

/* Looks key up. Completes from the event loop, never inline. An empty key is
 * an error (EINVAL), reported inline. */
async_next c_store_get_entry(const c_store* store,
                             const char*    key,
                             c_get_slot*    out,
                             async_cont*    k,
                             async_sp       sp) ASYNC_NOEXCEPT;

/* Awaits c_store_get and completes with the value's length, or 0 if absent.
 * An asynchronous function that calls another, written in C. */
async_next c_store_get_len_entry(const c_store* store,
                                 const char*    key,
                                 c_len_slot*    out,
                                 async_cont*    k,
                                 async_sp       sp) ASYNC_NOEXCEPT;

#ifdef __cplusplus
}
#endif
#endif

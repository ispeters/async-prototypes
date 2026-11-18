/* An asynchronous function written in C, behind the ABI in abi4a.h. */
#ifndef C_STORE_H
#define C_STORE_H
#include "abi4a.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct c_store c_store;
c_store const* c_store_default(void);
int c_store_run_pending(void); /* the event loop; returns tasks run */

/* [[tony:c_header]] */
/* The C++ declaration this entry stands behind:
 *
 *   using c_lookup = function<
 *       sender_tag(c_store const*, char const*) noexcept,
 *       completion_signatures<set_value_t(c_value), set_error_t(int),
 *                             set_stopped_t()>>;
 *   c_lookup c_store_get(c_store const* store, char const* key) noexcept;
 */
typedef struct c_value {
  char const* data; /* NULL when the key is absent */
  size_t len;
} c_value;

enum { C_VALUE = 0, C_ERROR = 1, C_STOPPED = 2 };

async_next c_store_get_entry(c_store const* store, char const* key,
                             async_cont* k, async_sp sp) ASYNC_NOEXCEPT;
/* [[/tony]] */

#ifdef __cplusplus
}
#endif
#endif

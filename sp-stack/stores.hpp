// `kv_store::get` three ways, and two callers of it. A caller sees only this
// header; the stores are separately compiled and reached through virtual calls.
#pragma once

#include "common.hpp"
#include "mstack.hpp"

// The synchronous function, as the yardstick.
struct kv_store {
    virtual ~kv_store()                     = default;
    virtual value_t get_sync(str key) const = 0;
};

// Cursor: kv-handle's visible entry, unchanged. Frames go on a separate async
// stack whose cursor is a parameter.
struct kv_store_v {
    virtual ~kv_store_v()                                          = default;
    virtual async_next get_entry(str&           key,
                                 slot<value_t>* out,
                                 async_cont*    k,
                                 async_sp       sp) const noexcept = 0;
};

// Machine stack: the same entry without the cursor. The callee's frame is its
// stack frame.
struct kv_store_m {
    virtual ~kv_store_m() = default;
    virtual ms::next
    get_entry(str& key, slot<value_t>* out, ms::cont* k) const noexcept = 0;
};

struct stores {
    kv_store*   sync;
    kv_store_v* cursor;
    kv_store_m* machine;
};
// A cache over a backing store. With `deferred` the backing store completes
// from the event loop.
stores make_stores(bool retain, bool deferred);

// ---- a caller that awaits in a loop -----------------------------------------
//
//   async size_t count_found(kv_store const& s, str const& key, long n) {
//     size_t total = 0;
//     for (long i = 0; i < n; ++i)
//       if (co_await s.get(key)) ++total;
//     co_return total;
//   }
std::size_t count_found_sync(const kv_store& s, const str& key, long n);
async_next  count_found_c(const kv_store_v&  s,
                          const str&         key,
                          long               n,
                          slot<std::size_t>* out,
                          async_cont*        k,
                          async_sp           sp) noexcept;
ms::next    count_found_m(const kv_store_m&  s,
                          const str&         key,
                          long               n,
                          slot<std::size_t>* out,
                          ms::cont*          k) noexcept;

// ---- a caller that forks ------------------------------------------------------
//
//   async size_t count_all(kv_store const& s, str const* keys, size_t n) {
//     // n concurrent gets, joined; the number that found a value
//   }
//
// Machine stack: every child runs on a stack of its own from `stacks`.
ms::next count_all_m(const kv_store_m&   s,
                     const str*          keys,
                     std::size_t         n,
                     ms::stack_provider& stacks,
                     slot<std::size_t>*  out,
                     ms::cont*           k) noexcept;
// Cursor: every child gets a strand of its own (one 4 KiB slab from malloc).
// Nothing smarter has been built for the cursor design.
async_next count_all_c(const kv_store_v&  s,
                       const str*         keys,
                       std::size_t        n,
                       slot<std::size_t>* out,
                       async_cont*        k,
                       async_sp           sp) noexcept;

// Statistics.
// mem, cache, count_found: sizeof the hand-written record
extern std::size_t machine_frame_bytes[3];
extern std::size_t cursor_frame_bytes[3];

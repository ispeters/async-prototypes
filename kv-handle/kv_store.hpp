// `kv_store::get` behind three interfaces. A caller sees only this header; the
// stores are separately compiled and reached through virtual calls.
#pragma once

#include "handle.hpp"

using get_fn = async_fn<value_t(str) const &>;   // P4223R2's `function`
using hget_fn = hfn<value_t(str) const &>;       // `function` with a `start` that returns a handle

// Today: P4223R2's declaration.
struct kv_store {
  virtual ~kv_store() = default;
  virtual get_fn get(str key) const = 0;
  // The synchronous function `get` parallels, as the yardstick.
  virtual value_t get_sync(str key) const = 0;
};

// The entry hidden: the same declaration, with the other `function`. The entry
// is an internal function of the implementation file.
struct kv_store_h {
  virtual ~kv_store_h() = default;
  virtual hget_fn get(str key) const = 0;
};

// The entry visible: what a compiler could declare beside `get` so that a call
// is one dispatch and makes no sender. `key` is the caller's parameter object,
// as a by-value class parameter is under the Itanium ABI.
struct kv_store_v {
  virtual ~kv_store_v() = default;
  virtual async_next get_entry(str& key, slot<value_t>* out, async_cont* k, async_sp sp) const noexcept = 0;
};

// Today's `function` again, declared so that it forwards something through its
// erasure by type: the strand as its frame allocator, or the cursor.
template <class... Queries>
using get_fn_with = exec::function<
    ex::sender_tag(str) const &,
    ex::completion_signatures<ex::set_value_t(value_t), ex::set_error_t(std::exception_ptr),
                              ex::set_stopped_t()>,
    exec::queries<Queries...>>;
using get_fn_alloc = get_fn_with<strand_allocator<std::byte>(exec::get_frame_allocator_t) noexcept>;
using get_fn_cursor = get_fn_with<async_sp*(get_async_stack_t) noexcept>;

template <class Fn>
struct kv_iface {
  virtual ~kv_iface() = default;
  virtual Fn get(str key) const = 0;
};
using kv_store_alloc = kv_iface<get_fn_alloc>;
using kv_store_cursor = kv_iface<get_fn_cursor>;

// A cache over a backing store. Exactly one member is set: the interface the
// cache is reached through.
struct stores {
  kv_store* today = nullptr;
  kv_store_h* hidden = nullptr;
  kv_store_v* visible = nullptr;
  kv_store_alloc* alloc = nullptr;
  kv_store_cursor* cursor = nullptr;
};

// cache: 'p' published, 'l' lowered to a C++26 sender (both behind kv_store);
//        'n' handle-aware, 'o' handle-aware and owning its key, 'w' the
//        published definition unchanged (all behind kv_store_h); 'v' visible.
// mem:   the same letters; 'p' and 'l' are reached through kv_store, 'n', 'o'
//        and 'w' through kv_store_h, 'v' through kv_store_v.
//        'a' and 'c' are the published definitions behind kv_store_alloc
//        and kv_store_cursor, over a backing store of the same letter or 'n'.
// A 'p' or 'n' cache can sit over a backing store behind either of the first
// two interfaces.
stores make_stores(char cache_kind, char mem_kind, bool retain, bool deferred);

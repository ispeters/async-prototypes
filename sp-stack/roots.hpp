// The roots of a call tree: what calls an entry from ordinary code and drives
// the operation to completion.
#pragma once

#include "stores.hpp"

struct root_c : async_cont {
  int how = ASYNC_PENDING;
  root_c() {
    resume = [](async_cont* c, int h) noexcept -> async_next {
      static_cast<root_c*>(c)->how = h;
      return {nullptr, ASYNC_PENDING};
    };
  }
  void drive(async_next n) noexcept {
    if (n.next == this) how = n.how;
    else async_run(n);
    while (how == ASYNC_PENDING) async_run_pending();
  }
};

struct root_m : ms::cont {
  int how = ASYNC_PENDING;
  explicit root_m(void* stack_top) {
    resume = [](ms::cont* c, int h) noexcept -> ms::next {
      static_cast<root_m*>(c)->how = h;
      return {nullptr, ASYNC_PENDING};
    };
    sp = stack_top;  // the strand is empty by the time the root is told
  }
  void drive(ms::next n) noexcept {
    if (n.k == this) how = n.how;
    else ms::run(n);
    while (how == ASYNC_PENDING) ms::run_pending();
  }
};

#define ON_STRAND [&] [[gnu::always_inline]] () noexcept

// ---- one get ----------------------------------------------------------------
[[gnu::noinline]] inline std::size_t call_sync(kv_store const& s, str const& key) {
  value_t v = s.get_sync(key);
  return v ? 1 : 0;
}

[[gnu::noinline]] inline std::size_t call_cursor(kv_store_v const& s, str const& key, async_sp sp) {
  str arg(key);
  slot<value_t> out;
  root_c k;
  k.drive(s.get_entry(arg, &out, &k, sp));
  std::size_t r = (k.how == ASYNC_VALUE && out.value) ? 1 : 0;
  out.destroy(k.how);
  return r;
}

[[gnu::noinline]] inline std::size_t call_machine(kv_store_m const& s, str const& key, ms::stack stk) {
  str arg(key);
  slot<value_t> out;
  root_m k(stk.top());
  k.drive(ms::enter(stk.top(), &k, ON_STRAND { return s.get_entry(arg, &out, &k); }));
  std::size_t r = (k.how == ASYNC_VALUE && out.value) ? 1 : 0;
  out.destroy(k.how);
  return r;
}

// ---- n gets in a loop, under one root ----------------------------------------
[[gnu::noinline]] inline std::size_t loop_cursor(kv_store_v const& s, str const& key, long n, async_sp sp) {
  slot<std::size_t> out;
  root_c k;
  k.drive(count_found_c(s, key, n, &out, &k, sp));
  return out.value;
}

[[gnu::noinline]] inline std::size_t loop_machine(kv_store_m const& s, str const& key, long n, ms::stack stk) {
  slot<std::size_t> out;
  root_m k(stk.top());
  k.drive(ms::enter(stk.top(), &k, ON_STRAND { return count_found_m(s, key, n, &out, &k); }));
  return out.value;
}

// ---- n concurrent gets, joined -----------------------------------------------
[[gnu::noinline]] inline std::size_t fork_cursor(kv_store_v const& s, str const* keys, std::size_t n, async_sp sp) {
  slot<std::size_t> out;
  root_c k;
  k.drive(count_all_c(s, keys, n, &out, &k, sp));
  return out.value;
}

[[gnu::noinline]] inline std::size_t fork_machine(kv_store_m const& s, str const* keys, std::size_t n,
                                                 ms::stack_provider& stacks, ms::stack stk) {
  slot<std::size_t> out;
  root_m k(stk.top());
  k.drive(ms::enter(stk.top(), &k, ON_STRAND { return count_all_m(s, keys, n, stacks, &out, &k); }));
  return out.value;
}

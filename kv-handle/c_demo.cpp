// A C++ caller of asynchronous functions implemented in C (c_store.c), first
// through the entry and then as a sender composed with a standard algorithm.
#include "c_store.h"

#include <stdexec/execution.hpp>

#include <cerrno>
#include <cstdio>
#include <utility>

namespace ex = STDEXEC;

// ---- as a sender: what c_store_get_len(store, key) returns in C++ -----------
struct c_get_len_sender {
  using sender_concept = ex::sender_tag;
  using completion_signatures = ex::completion_signatures<
      ex::set_value_t(std::size_t), ex::set_error_t(int), ex::set_stopped_t()>;
  c_store const* store;
  char const* key;

  template <class R>
  struct op : async_cont {
    using operation_state_concept = ex::operation_state_tag;
    R rcvr;
    c_store const* store;
    char const* key;
    c_len_slot out;
    async_sp root = nullptr;
    op(R r, c_store const* s, char const* k) : rcvr(std::move(r)), store(s), key(k) {
      resume = [](async_cont* c, int how) noexcept -> async_next {
        static_cast<op*>(c)->complete(how);
        return {nullptr, ASYNC_PENDING};
      };
    }
    op(op&&) = delete;
    ~op() { if (root) async_stack_destroy(root); }
    void start() & noexcept {
      root = async_stack_create();
      async_next n = c_store_get_len_entry(store, key, &out, this, root);
      if (n.next == this) complete(n.how);
      else async_run(n);
    }
    void complete(int how) noexcept {
      if (how == ASYNC_VALUE) ex::set_value(std::move(rcvr), std::size_t(out.value));
      else if (how == ASYNC_ERROR) ex::set_error(std::move(rcvr), int(out.error));
      else ex::set_stopped(std::move(rcvr));
    }
  };
  template <ex::receiver R>
  op<R> connect(R r) const { return op<R>(std::move(r), store, key); }
};

struct print_rcvr {
  using receiver_concept = ex::receiver_tag;
  char const* what;
  bool* done;
  void set_value(std::size_t n) && noexcept { std::printf("%s: value %zu\n", what, n); *done = true; }
  void set_error(int e) && noexcept { std::printf("%s: error %d%s\n", what, e, e == EINVAL ? " (EINVAL)" : ""); *done = true; }
  void set_stopped() && noexcept { std::printf("%s: stopped\n", what); *done = true; }
};

struct root_cont : async_cont {
  int how = ASYNC_PENDING;
  root_cont() {
    resume = [](async_cont* c, int h) noexcept -> async_next {
      static_cast<root_cont*>(c)->how = h;
      return {nullptr, ASYNC_PENDING};
    };
  }
};

int main() {
  c_store* store = c_store_open();

  // 1. through the entry, from the root of a call tree
  async_sp sp = async_stack_create();
  for (char const* key : {"k2", "nope", ""}) {
    c_len_slot out;
    root_cont k;
    async_next n = c_store_get_len_entry(store, key, &out, &k, sp);
    bool inline_ = n.next == &k;
    if (inline_) k.how = n.how;
    else async_run(n);
    while (k.how == ASYNC_PENDING) async_run_pending();
    if (k.how == ASYNC_VALUE) std::printf("entry  get_len(\"%s\"): value %zu (%s)\n", key, out.value, inline_ ? "inline" : "after suspending");
    else std::printf("entry  get_len(\"%s\"): error %d (%s)\n", key, out.error, inline_ ? "inline" : "after suspending");
  }
  async_stack_destroy(sp);

  // 2. as a sender, composed with then
  for (char const* key : {"k2", ""}) {
    bool done = false;
    auto op = ex::connect(c_get_len_sender{store, key} | ex::then([](std::size_t n) noexcept { return n * 2; }),
                          print_rcvr{"sender get_len | then(n * 2)", &done});
    ex::start(op);
    while (!done) async_run_pending();
  }
}

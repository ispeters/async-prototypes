#include "kv_store.hpp"

#include <exec/variant_sender.hpp>

namespace {

// ============================================================================
// The backing store's data, and a sender that completes inline or, when
// `deferred`, from the event loop
// ============================================================================
struct mem_data {
  std::map<str, str> data;
  bool deferred;
  value_t find(str const& key) const {
    if (auto it = data.find(key); it != data.end()) return it->second;
    return std::nullopt;
  }
};

struct maybe_defer {
  using sender_concept = ex::sender_tag;
  using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
  bool deferred;

  template <class R>
  struct op : async_task {
    using operation_state_concept = ex::operation_state_tag;
    R rcvr;
    bool deferred;
    op(R r, bool d) : rcvr(std::move(r)), deferred(d) {}
    op(op&&) = delete;
    void start() & noexcept {
      if (!deferred) return ex::set_value(std::move(rcvr));
      run = [](async_task* t) noexcept -> async_next {
        ex::set_value(std::move(static_cast<op*>(t)->rcvr));
        return {nullptr, ASYNC_PENDING};
      };
      async_defer(this);
    }
  };
  template <ex::receiver R>
  op<R> connect(R r) const { return op<R>(std::move(r), deferred); }
};

// ============================================================================
// PUBLISHED: P4223R2's definitions (fn/mem_store.cpp, cached_lookup.hpp and
// fn/cache.cpp), over either `function`
// ============================================================================
template <class Store>
auto cached_lookup(Store const& backing, cache_entries& entries, str key) {
  return ex::just(std::move(key)) |
         ex::let_value([&backing, &entries](str const& key) {
           auto miss = [&] {
             return backing.get(key) |
                    ex::then([&entries, &key](value_t value) {
                      if (value) {
                        entries.insert(key, *value);
                      }
                      return value;
                    });
           };
           // a hit and a miss are different senders, so return a variant
           using lookup = exec::variant_sender<decltype(ex::just(value_t())), decltype(miss())>;
           if (auto value = entries.find(key)) {
             return lookup(ex::just(std::move(value)));
           }
           return lookup(miss());
         });
}

struct mem_p final : kv_store, mem_data {
  mem_p(mem_data d) : mem_data(std::move(d)) {}
  get_fn get(str key) const override {
    return get_fn(*this, std::move(key), [](mem_p const& self, str key) {
      return maybe_defer{self.deferred} |
             ex::then([&self, key = std::move(key)] { return self.find(key); });
    });
  }
  value_t get_sync(str key) const override { return find(key); }
};

template <class Backing>
struct cache_p final : kv_store {
  Backing const* backing_;
  mutable cache_entries entries_;
  cache_p(Backing const& b, bool retain) : backing_(&b), entries_(retain) {}

  get_fn get(str key) const override {
    return get_fn(*this, std::move(key), [](cache_p const& self, str key) {
      return cached_lookup(*self.backing_, self.entries_, std::move(key));
    });
  }

  // The synchronous function the asynchronous ones parallel.
  value_t get_sync(str key) const override {
    if constexpr (requires { backing_->get_sync(key); }) {
      if (auto hit = entries_.find(key))
        return hit;
      auto value = backing_->get_sync(key);
      if (value)
        entries_.insert(key, *value);
      return value;
    } else {
      std::abort();
    }
  }
};

// The same two definitions behind today's `function` declared with a query.
template <class Fn>
struct mem_q final : kv_iface<Fn>, mem_data {
  mem_q(mem_data d) : mem_data(std::move(d)) {}
  Fn get(str key) const override {
    return Fn(*this, std::move(key), [](mem_q const& self, str key) {
      return maybe_defer{self.deferred} |
             ex::then([&self, key = std::move(key)] { return self.find(key); });
    });
  }
};

template <class Fn, class Backing>
struct cache_q final : kv_iface<Fn> {
  Backing const* backing_;
  mutable cache_entries entries_;
  cache_q(Backing const& b, bool retain) : backing_(&b), entries_(retain) {}
  Fn get(str key) const override {
    return Fn(*this, std::move(key), [](cache_q const& self, str key) {
      return cached_lookup(*self.backing_, self.entries_, std::move(key));
    });
  }
};

// The same two definitions with `hget_fn` for `get_fn`: published code
// recompiled against the other `function`.
struct mem_w final : kv_store_h, mem_data {
  mem_w(mem_data d) : mem_data(std::move(d)) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](mem_w const& self, str key) {
      return maybe_defer{self.deferred} |
             ex::then([&self, key = std::move(key)] { return self.find(key); });
    });
  }
};

struct cache_w final : kv_store_h {
  kv_store_h const* backing_;
  mutable cache_entries entries_;
  cache_w(kv_store_h const& b, bool retain) : backing_(&b), entries_(retain) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](cache_w const& self, str key) {
      return cached_lookup(*self.backing_, self.entries_, std::move(key));
    });
  }
};

// ============================================================================
// LOWERED TO A C++26 SENDER, behind today's `function`. The bodies:
//
//   get_fn mem_store::get(str key) const {
//     co_await maybe_defer{deferred};
//     co_return find(key);
//   }
//
//   get_fn cache::get(str key) const {
//     if (auto hit = entries_.find(key))
//       co_return hit;
//     auto value = co_await backing_->get(key);
//     if (value)
//       entries_.insert(key, *value);
//     co_return value;
//   }
// ============================================================================
struct mem_get_sender {
  using sender_concept = ex::sender_tag;
  using completion_signatures = ex::completion_signatures<ex::set_value_t(value_t)>;
  mem_data const* self;
  str key;

  template <class R>
  struct op : async_task {
    using operation_state_concept = ex::operation_state_tag;
    R rcvr;
    mem_data const* self;
    str key;
    op(R r, mem_data const* s, str k) : rcvr(std::move(r)), self(s), key(std::move(k)) {}
    op(op&&) = delete;
    void start() & noexcept {
      if (!self->deferred) return finish();
      run = [](async_task* t) noexcept -> async_next {
        static_cast<op*>(t)->finish();
        return {nullptr, ASYNC_PENDING};
      };
      async_defer(this);
    }
    void finish() noexcept {
      // co_return of a prvalue: built on this call's stack, then delivered
      ex::set_value(std::move(rcvr), self->find(key));
    }
  };
  template <ex::receiver R>
  op<R> connect(R r) && { return op<R>(std::move(r), self, std::move(key)); }
};

struct mem_l final : kv_store, mem_data {
  mem_l(mem_data d) : mem_data(std::move(d)) {}
  get_fn get(str key) const override {
    return get_fn(*this, std::move(key), [](mem_l const& self, str key) {
      return mem_get_sender{&self, std::move(key)};
    });
  }
  value_t get_sync(str key) const override { return find(key); }
};

struct cache_l final : kv_store {
  kv_store const* backing_;
  mutable cache_entries entries_;
  cache_l(kv_store const& b, bool retain) : backing_(&b), entries_(retain) {}

  template <class R>
  struct op {
    using operation_state_concept = ex::operation_state_tag;

    struct await_rcvr {
      using receiver_concept = ex::receiver_tag;
      op* o;
      void set_value(value_t&& v) && noexcept {
        ::new (&o->value) value_t(std::move(v));  // the await's result becomes `value`
        if (tl_starting.op == o) *tl_starting.how = ASYNC_VALUE;
        else o->resume();
      }
      // Errors and stopped aren't handled by the body, so they're forwarded: copy
      // the payload out of the child, release the child, complete.
      void set_error(std::exception_ptr e) && noexcept {
        if (tl_starting.op == o) *tl_starting.how = ASYNC_ERROR;
        o->child.~child_t();
        ex::set_error(std::move(o->rcvr), std::move(e));
      }
      void set_stopped() && noexcept {
        if (tl_starting.op == o) *tl_starting.how = ASYNC_STOPPED;
        o->child.~child_t();
        ex::set_stopped(std::move(o->rcvr));
      }
      ex::env_of_t<R> get_env() const noexcept { return ex::get_env(o->rcvr); }
    };
    using child_t = ex::connect_result_t<get_fn, await_rcvr>;

    R rcvr;
    cache_l const* self;
    str key;                  // parameter: destroyed with the operation state
    union { value_t value; }; // local, alive from the await to the co_return
    union { child_t child; }; // the awaited operation, alive while outstanding

    op(R r, cache_l const* s, str k) : rcvr(std::move(r)), self(s), key(std::move(k)) {}
    op(op&&) = delete;
    ~op() {}

    void start() & noexcept {
      if (auto hit = self->entries_.find(key)) {
        // co_return hit: `hit` is treated as the return object (the elision the
        // design permits for a named local)
        return ex::set_value(std::move(rcvr), std::move(hit));
      }
      ::new (&child) child_t(ex::connect(self->backing_->get(key), await_rcvr{this}));
      int how = ASYNC_PENDING;
      auto saved = std::exchange(tl_starting, start_guard{this, &how});
      ex::start(child);
      tl_starting = saved;
      if (how == ASYNC_VALUE) resume();
      // pending: await_rcvr resumes the body later. Forwarded: nothing left to do.
    }

    void resume() noexcept {
      child.~child_t();  // an awaited operation is released when it completes
      if (value) self->entries_.insert(key, *value);
      // co_return value: `value` lives in this frame, which the completion may
      // destroy, so it's moved out first and destroyed with the other locals
      value_t ret(std::move(value));
      value.~value_t();
      ex::set_value(std::move(rcvr), std::move(ret));
    }
  };

  struct sender {
    using sender_concept = ex::sender_tag;
    using completion_signatures = ex::completion_signatures<
        ex::set_value_t(value_t), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;
    cache_l const* self;
    str key;
    template <ex::receiver R>
    op<R> connect(R r) && { return op<R>(std::move(r), self, std::move(key)); }
  };

  get_fn get(str key) const override {
    return get_fn(*this, std::move(key), [](cache_l const& self, str key) {
      return sender{&self, std::move(key)};
    });
  }

  value_t get_sync(str) const override { std::abort(); }
};

// ============================================================================
// LOWERED TO AN ENTRY: the same two bodies, once each. Results are constructed
// in the caller's slot, arguments are borrowed from the caller, and a frame is
// pushed on the async stack only on the path that needs one.
// ============================================================================
struct mem_frame : async_task {
  mem_data const* self;
  str* key;  // the caller's parameter object: valid until we complete
  slot<value_t>* out;
  async_cont* k;
};

[[gnu::always_inline]] inline async_next mem_body(mem_data const& self, str& key, slot<value_t>* out,
                                                  async_cont* k, async_sp sp) noexcept {
  if (!self.deferred) {
    // co_return of a prvalue, constructed in the caller's storage
    ::new (&out->value) value_t(self.find(key));
    return {k, ASYNC_VALUE};  // completing is handing back the caller's record
  }
  async_sp top = sp;
  auto* f = ::new (async_push(&top, sizeof(mem_frame))) mem_frame;
  f->self = &self;
  f->key = &key;
  f->out = out;
  f->k = k;
  f->run = [](async_task* t) noexcept -> async_next {
    auto* f = static_cast<mem_frame*>(t);
    ::new (&f->out->value) value_t(f->self->find(*f->key));
    async_cont* up = f->k;
    async_sp top;
    async_pop(&top, f);  // the frame is gone before the caller continues
    return {up, ASYNC_VALUE};
  };
  async_defer(f);
  return {nullptr, ASYNC_PENDING};
}

// How `co_await backing_->get(key)` lowers, by the interface of `backing_`:
// what temporary of the full-expression the frame holds, and how it's started.
template <class Backing>
struct await_get;

template <>
struct await_get<kv_store_v> {  // one call; the temporary is the callee's parameter object
  using temp = str;
  static void make(temp* t, kv_store_v const&, str const& key) { ::new (t) str(key); }
  static async_next start(kv_store_v const& b, temp& t, str const&, slot<value_t>* out, async_cont* k,
                          async_sp sp) noexcept {
    return b.get_entry(t, out, k, sp);
  }
};

template <>
struct await_get<kv_store_h> {  // make the sender, connect it, start it
  using temp = hget_fn;
  static void make(temp* t, kv_store_h const& b, str const& key) { ::new (t) hget_fn(b.get(key)); }
  static async_next start(kv_store_h const&, temp& t, str const&, slot<value_t>* out, async_cont* k,
                          async_sp sp) noexcept {
    return t.connect(handle_rcvr<value_t>{out, k, sp}).start();
  }
};

template <>
struct await_get<kv_store> {  // a C++26 sender: the thunk connects it in a frame of its own
  struct temp {};
  static void make(temp*, kv_store const&, str const&) {}
  static async_next start(kv_store const& b, temp&, str const& key, slot<value_t>* out, async_cont* k,
                          async_sp sp) noexcept {
    return hconnect(b.get(key), handle_rcvr<value_t>{out, k, sp}).start();
  }
};

template <class Backing>
struct cache_frame : async_cont {
  using temp = typename await_get<Backing>::temp;
  cache_entries* entries;
  str* key;            // parameter, in the caller
  slot<value_t>* out;  // where the result goes, in the caller; also the local `value`
  async_cont* k;       // who runs next when we complete
  union { temp t; };
  cache_frame() noexcept {}
  ~cache_frame() {}

  // The rest of the body, after the await. It exists once: it's reached by
  // falling through, or as this frame's `resume`.
  async_next after_await(int how) noexcept {
    t.~temp();  // end of the full-expression
    if (how == ASYNC_VALUE) {
      value_t& v = out->value;
      if (v) entries->insert(*key, *v);
    }
    async_cont* up = k;
    async_sp top;
    async_pop(&top, this);
    return {up, how};
  }
};

template <class Backing>
[[gnu::always_inline]] inline async_next cache_body(cache_entries& entries, Backing const& backing, str& key,
                                                    slot<value_t>* out, async_cont* k, async_sp sp) noexcept {
  using frame = cache_frame<Backing>;
  // `hit` is the return object: it's constructed in the caller's slot
  value_t& hit = *::new (&out->value) value_t(entries.find(key));
  if (hit) return {k, ASYNC_VALUE};  // co_return hit
  hit.~value_t();
  async_sp top = sp;
  auto* f = ::new (async_push(&top, sizeof(frame))) frame;
  f->entries = &entries;
  f->key = &key;
  f->out = out;
  f->k = k;
  f->resume = [](async_cont* c, int how) noexcept { return static_cast<frame*>(c)->after_await(how); };
  await_get<Backing>::make(&f->t, backing, key);
  async_next n = await_get<Backing>::start(backing, f->t, key, out, f, top);
  // Being handed our own record means "you're next": carry on here and skip
  // the trip through the bottom of the stack.
  if (n.next == f) return f->after_await(n.how);
  return n;
}

// ---- VISIBLE: the entry is the virtual function ------------------------------
struct mem_v final : kv_store_v, mem_data {
  mem_v(mem_data d) : mem_data(std::move(d)) {}
  async_next get_entry(str& key, slot<value_t>* out, async_cont* k, async_sp sp) const noexcept override {
    return mem_body(*this, key, out, k, sp);
  }
};

struct cache_v final : kv_store_v {
  kv_store_v const* backing_;
  mutable cache_entries entries_;
  cache_v(kv_store_v const& b, bool retain) : backing_(&b), entries_(retain) {}
  async_next get_entry(str& key, slot<value_t>* out, async_cont* k, async_sp sp) const noexcept override {
    return cache_body(entries_, *backing_, key, out, k, sp);
  }
};

// ---- HIDDEN: `get` returns a `function` made as P4223's is -------------------
// The factory takes the curried key by reference, and the sender it returns
// only points at things, so nothing has to be kept for the operation.
struct mem_get {
  mem_data const* self;
  str* key;
  struct op {
    mem_data const* self;
    str* key;
    handle_rcvr<value_t> r;
    async_next start() noexcept { return mem_body(*self, *key, r.out, r.k, r.sp); }
  };
  op connect(handle_rcvr<value_t> r) const noexcept { return {self, key, r}; }
};

template <class Backing>
struct cache_get {
  cache_entries* entries;
  Backing const* backing;
  str* key;
  struct op {
    cache_entries* entries;
    Backing const* backing;
    str* key;
    handle_rcvr<value_t> r;
    async_next start() noexcept { return cache_body(*entries, *backing, *key, r.out, r.k, r.sp); }
  };
  op connect(handle_rcvr<value_t> r) const noexcept { return {entries, backing, key, r}; }
};

struct mem_n final : kv_store_h, mem_data {
  mem_n(mem_data d) : mem_data(std::move(d)) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](mem_n const& self, str& key) noexcept {
      return mem_get{&self, &key};
    });
  }
};

template <class Backing>
struct cache_n final : kv_store_h {
  Backing const* backing_;
  mutable cache_entries entries_;
  cache_n(Backing const& b, bool retain) : backing_(&b), entries_(retain) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](cache_n const& self, str& key) noexcept {
      return cache_get<Backing>{&self.entries_, self.backing_, &key};
    });
  }
};

// ---- HIDDEN, with the factory written as P4223 publishes it ------------------
// The factory takes the key by value and the sender owns it, so the sender has
// to outlive the operation: `function` gives it a frame.
struct mem_get_owning {
  mem_data const* self;
  str key;
  mem_get::op connect(handle_rcvr<value_t> r) & noexcept { return {self, &key, r}; }
};

struct cache_get_owning {
  cache_entries* entries;
  kv_store_h const* backing;
  str key;
  cache_get<kv_store_h>::op connect(handle_rcvr<value_t> r) & noexcept { return {entries, backing, &key, r}; }
};

struct mem_o final : kv_store_h, mem_data {
  mem_o(mem_data d) : mem_data(std::move(d)) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](mem_o const& self, str key) noexcept {
      return mem_get_owning{&self, std::move(key)};
    });
  }
};

struct cache_o final : kv_store_h {
  kv_store_h const* backing_;
  mutable cache_entries entries_;
  cache_o(kv_store_h const& b, bool retain) : backing_(&b), entries_(retain) {}
  hget_fn get(str key) const override {
    return hget_fn(*this, std::move(key), [](cache_o const& self, str key) noexcept {
      return cache_get_owning{&self.entries_, self.backing_, std::move(key)};
    });
  }
};

}  // namespace

stores make_stores(char cache_kind, char mem_kind, bool retain, bool deferred) {
  mem_data data{{{"k1", "v1"}, {"k2", "v2"}}, deferred};
  kv_store* today = nullptr;
  kv_store_h* hidden = nullptr;
  kv_store_v* visible = nullptr;
  kv_store_alloc* alloc = nullptr;
  kv_store_cursor* cursor = nullptr;
  switch (mem_kind) {
  case 'p': today = new mem_p(data); break;
  case 'l': today = new mem_l(data); break;
  case 'n': hidden = new mem_n(data); break;
  case 'o': hidden = new mem_o(data); break;
  case 'w': hidden = new mem_w(data); break;
  case 'v': visible = new mem_v(data); break;
  case 'a': alloc = new mem_q<get_fn_alloc>(data); break;
  case 'c': cursor = new mem_q<get_fn_cursor>(data); break;
  default: std::abort();
  }
  stores s;
  switch (cache_kind) {
  case 'p':
    if (today) s.today = new cache_p<kv_store>(*today, retain);
    else if (hidden) s.today = new cache_p<kv_store_h>(*hidden, retain);
    break;
  case 'l':
    if (today) s.today = new cache_l(*today, retain);
    break;
  case 'n':
    if (hidden) s.hidden = new cache_n<kv_store_h>(*hidden, retain);
    else if (today) s.hidden = new cache_n<kv_store>(*today, retain);
    break;
  case 'o':
    if (hidden) s.hidden = new cache_o(*hidden, retain);
    break;
  case 'w':
    if (hidden) s.hidden = new cache_w(*hidden, retain);
    break;
  case 'v':
    if (visible) s.visible = new cache_v(*visible, retain);
    break;
  case 'a':
    if (alloc) s.alloc = new cache_q<get_fn_alloc, kv_store_alloc>(*alloc, retain);
    else if (hidden) s.alloc = new cache_q<get_fn_alloc, kv_store_h>(*hidden, retain);
    break;
  case 'c':
    if (cursor) s.cursor = new cache_q<get_fn_cursor, kv_store_cursor>(*cursor, retain);
    else if (hidden) s.cursor = new cache_q<get_fn_cursor, kv_store_h>(*hidden, retain);
    break;
  }
  if (!s.today && !s.hidden && !s.visible && !s.alloc && !s.cursor) std::abort();
  return s;
}

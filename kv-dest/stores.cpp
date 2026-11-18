// The erased definitions. A caller sees only bodies.hpp's `store<V>`.
#include "bodies.hpp"

template <class V>
typename V::handle store<V>::get(str&, D<value_t>, R*, async_sp) const noexcept {
  std::abort();
}
template <class V>
typename V::handle store<V>::fetch(str&, D<str>, R*, async_sp) const noexcept {
  std::abort();
}
template <class V>
typename V::handle store<V>::get_or(str&, str&, D<str>, R*, async_sp) const noexcept {
  std::abort();
}
template <class V>
value_t store<V>::get_sync(str) const {
  std::abort();
}
template <class V>
str store<V>::get_or_sync(str, str) const {
  std::abort();
}

namespace {

template <class V>
struct mem final : store<V>, mem_data {
  using typename store<V>::H;
  using typename store<V>::R;
  template <class T>
  using D = typename store<V>::template D<T>;
  explicit mem(mem_data d) : mem_data(std::move(d)) {}
  H get(str& key, D<value_t> d, R* k, async_sp sp) const noexcept override {
    return leaf_body<V, value_t, false>(*this, key, d, k, sp);
  }
  H fetch(str& key, D<str> d, R* k, async_sp sp) const noexcept override {
    return leaf_body<V, str, true>(*this, key, d, k, sp);
  }
  value_t get_sync(str key) const override { return find(key); }
  str fetch_sync(str const& key) const {
    if (str const* v = lookup(key)) return *v;
    std::rethrow_exception(g_not_found);
  }
};

template <class V>
struct cache final : store<V> {
  using typename store<V>::H;
  using typename store<V>::R;
  template <class T>
  using D = typename store<V>::template D<T>;
  store<V> const* backing_;
  mutable cache_entries entries_;
  cache(store<V> const& b, bool retain) : backing_(&b), entries_(retain) {}
  H get(str& key, D<value_t> d, R* k, async_sp sp) const noexcept override {
    return cache_body<V>(entries_, *backing_, key, d, k, sp);
  }
  // The synchronous function the asynchronous one parallels, as the yardstick.
  value_t get_sync(str key) const override {
    if (auto hit = entries_.find(key)) return hit;
    auto value = backing_->get_sync(key);
    if (value) entries_.insert(key, *value);
    return value;
  }
};

template <class V>
struct get_or_store final : store<V> {
  using typename store<V>::H;
  using typename store<V>::R;
  template <class T>
  using D = typename store<V>::template D<T>;
  store<V> const* backing_;
  explicit get_or_store(store<V> const& b) : backing_(&b) {}
  H get_or(str& key, str& fb, D<str> d, R* k, async_sp sp) const noexcept override {
    return filter_body<V, str>(link<V, str>{d, k}, fb, key,
                               [this](str& arg, auto cd, R* ck, async_sp top) noexcept {
                                 return backing_->fetch(arg, cd, ck, top);
                               },
                               sp);
  }
  // The value path of the synchronous equivalent (its error path throws).
  str get_or_sync(str key, str fb) const override {
    auto const* m = static_cast<mem<V> const*>(backing_);
    try {
      return m->fetch_sync(key);
    } catch (...) {
      return fb;
    }
  }
};

template <class V>
struct pass_store final : store<V> {
  using typename store<V>::H;
  using typename store<V>::R;
  template <class T>
  using D = typename store<V>::template D<T>;
  store<V> const* next_;
  mutable long count_ = 0;
  explicit pass_store(store<V> const& n) : next_(&n) {}
  H get_or(str& key, str& fb, D<str> d, R* k, async_sp sp) const noexcept override {
    return pass_body<V>(count_, *next_, key, fb, d, k, sp);
  }
  str get_or_sync(str key, str fb) const override {
    str r = next_->get_or_sync(std::move(key), std::move(fb));
    ++count_;
    return r;
  }
  long count() const noexcept override { return count_; }
};

}  // namespace

template <class V>
store<V>* make_store(char kind, store<V> const* below, bool retain, bool deferred) {
  switch (kind) {
  case 'm': return new mem<V>(mem_data{{{"k1", "v1"}, {"k2", "v2"}}, deferred});
  case 'c': return new cache<V>(*below, retain);
  case 'g': return new get_or_store<V>(*below);
  case 'p': return new pass_store<V>(*below);
  }
  std::abort();
}

#define INSTANTIATE(V)  \
  template struct store<V>; \
  template store<V>* make_store<V>(char, store<V> const*, bool, bool);
INSTANTIATE(V1A)
INSTANTIATE(V2A)
INSTANTIATE(V3A)
INSTANTIATE(V4A)
INSTANTIATE(V5A)
INSTANTIATE(V1B)
INSTANTIATE(V2B)
INSTANTIATE(V3B)
INSTANTIATE(V4B)
INSTANTIATE(V5B)

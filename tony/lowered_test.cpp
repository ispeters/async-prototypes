#include "lowered.hpp"
#include <cstdio>
#include <cstdlib>
#include <deque>
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

// a hand-written leaf: a map that completes inline, or later from a queue
struct pending { async_cont* k; std::string const* key; struct map_store const* self; };
std::deque<pending> queue;
struct map_store : kv_store {
  std::map<std::string, std::string> m;
  bool deferred = false;
  async_next complete_now(std::string const& key, async_cont* k) const noexcept {
    if (key == "bad") {
      ::new (async_destination_of(k, ERROR)(k)) std::error_code(std::make_error_code(std::errc::io_error));
      return async_complete(k, ERROR);
    }
    auto it = m.find(key);
    ::new (async_destination_of(k, VALUE)(k)) std::optional<std::string>(
        it == m.end() ? std::nullopt : std::optional<std::string>(it->second));
    return async_complete(k, VALUE);
  }
  async_next get_entry(std::string* key, async_cont* k, async_sp) const noexcept override {
    if (deferred) { queue.push_back({k, key, this}); return {}; }
    return complete_now(*key, k);
  }
};
void drain() {
  while (!queue.empty()) {
    auto p = queue.front(); queue.pop_front();
    async_run(p.self->complete_now(*p.key, p.k));
  }
}

// the root: a composed receiver, erased (section 9's erased_record)
struct root : async_cont {
  x29::slot<std::optional<std::string>> value;
  x29::slot<std::error_code> ec;
  int how = -1;
  static async_next resume(async_cont* c, int how) noexcept {
    auto* r = static_cast<root*>(c);
    r->how = how;
    if (how == VALUE) r->value.adopt();
    if (how == ERROR) r->ec.adopt();
    return {};
  }
  root();
};
constexpr x29::completion_table<2> root_table{
    &root::resume,
    {[](async_cont* c) noexcept -> void* { return static_cast<root*>(c)->value.address(); },
     [](async_cont* c) noexcept -> void* { return static_cast<root*>(c)->ec.address(); }}};
root::root() : async_cont{&root_table} {}

alignas(16) char stack[4096];

void get(kv_store const& s, std::string key, root& r) {
  async_next n = s.get_entry(&key, &r, stack);
  async_run(n);
  drain();
}

int main() {
  map_store backing;
  backing.m["a"] = "apple";
  cache c(backing);
  for (bool deferred : {false, true}) {
    backing.deferred = deferred;
    c.entries_.m.clear();
    { root r; get(c, "a", r); CHECK(r.how == VALUE && **r.value == "apple"); }       // miss
    CHECK(c.entries_.m.count("a"));
    { root r; get(c, "a", r); CHECK(r.how == VALUE && **r.value == "apple"); }       // hit
    { root r; get(c, "zz", r); CHECK(r.how == VALUE && !*r.value); }                 // absent
    { root r; get(c, "bad", r); CHECK(r.how == ERROR && *r.ec == std::errc::io_error); }
  }
  std::printf("lowered cache::get checks pass\n");
}

// What a compiler would emit for section 1's cache::get under 4A, written by
// hand. Regions between [[tony:NAME]] markers appear on the page verbatim.
#pragma once
#include "x29.hpp"

#include <map>
#include <optional>
#include <string>
#include <system_error>

enum { VALUE = 0, ERROR = 1, STOPPED = 2 };  // lookup's declared completions

inline std::error_code error_code_from(std::exception_ptr e) noexcept {
  try {
    std::rethrow_exception(e);
  } catch (std::bad_alloc const&) {
    return std::make_error_code(std::errc::not_enough_memory);
  } catch (std::system_error const& s) {
    return s.code();
  }
}

// [[tony:lowered_interface]]
// lookup kv_store::get(std::string key) const noexcept;  becomes, beside it,
struct kv_store {
  virtual ~kv_store() = default;
  // the entry: arguments, then the caller's record and its async stack
  virtual async_next get_entry(std::string* key, async_cont* k,
                               async_sp sp) const noexcept = 0;
};
// [[/tony]]

struct cache : kv_store {
  explicit cache(kv_store const& backing) : backing_(&backing) {}
  async_next get_entry(std::string* key, async_cont* k, async_sp sp) const noexcept override;

  struct table {
    std::optional<std::string> find(std::string const& k) const {
      auto it = m.find(k);
      if (it == m.end()) return std::nullopt;
      return it->second;
    }
    void insert(std::string const& k, std::string const& v) { m.insert_or_assign(k, v); }
    std::map<std::string, std::string> m;
  };
  mutable table entries_;
  kv_store const* backing_;
};

// [[tony:lowered_frame]]
// The body's frame: what lives across its one suspension.
struct cache_get_frame : async_cont {  // our record, as the child's caller
  async_cont* k;                       // our caller's record
  cache const* self;
  std::string* key;                    // our parameter, in our caller's storage
  std::optional<std::string> arg;      // backing_->get's argument
  union {
    std::optional<std::string> value;  // the local `value`
    std::error_code ec;                // the handler's `ec`
  };
  bool has_value = false;

  cache_get_frame(async_cont const& rec, async_cont* k, cache const* self,
                  std::string* key)
      : async_cont(rec), k(k), self(self), key(key), arg(std::in_place, *key) {}
  ~cache_get_frame() {
    if (has_value) value.~optional();
  }
};

async_next cache_get_resume(async_cont* c, int how) noexcept;

// Its table: where the child we await constructs each payload. These are
// the frame's own variables, so the answers are static.
inline constexpr x29::completion_table<2> cache_get_table{
    &cache_get_resume,
    {[](async_cont* c) noexcept -> void* {
       return &static_cast<cache_get_frame*>(c)->value;
     },
     [](async_cont* c) noexcept -> void* {
       return &static_cast<cache_get_frame*>(c)->ec;
     }}};

// Completes our caller with an error made from an exception.
inline async_next fail(async_cont* k, std::exception_ptr e) noexcept {
  ::new (async_destination_of(k, ERROR)(k)) std::error_code(error_code_from(e));
  return async_complete(k, ERROR);
}
// [[/tony]]

// [[tony:lowered_entry]]
async_next cache::get_entry(std::string* key, async_cont* k,
                            async_sp sp) const noexcept {
  try {
    // if (auto value = entries_.find(key)) co_return value;
    // Nothing has suspended yet: this runs on the machine stack, no frame.
    if (auto value = entries_.find(*key)) {
      ::new (async_destination_of(k, VALUE)(k))
          std::optional<std::string>(std::move(value));
      return async_complete(k, VALUE);
    }
  } catch (...) {
    return fail(k, std::current_exception());  // catch (...)
  }

  // The miss can suspend, so from here the body has a frame.
  void* mem = async_push(&sp, sizeof(cache_get_frame));
  cache_get_frame* f;
  try {
    f = ::new (mem) cache_get_frame({&cache_get_table}, k, this, key);
  } catch (...) {  // copying the key into the argument threw
    return fail(k, std::current_exception());
  }

  // auto value = co_await backing_->get(key);
  async_next n = backing_->get_entry(&*f->arg, f, sp);
  if (n.next == f) {
    return cache_get_resume(f, n.how);  // it completed inline: carry on here
  }
  return n;  // pending: whoever completes it runs us later
}

async_next cache_get_resume(async_cont* c, int how) noexcept {
  auto* f = static_cast<cache_get_frame*>(c);
  async_cont* k = f->k;
  f->arg.reset();  // the full-expression ends: the argument goes
  async_next done;
  switch (how) {
    case VALUE:
      f->has_value = true;
      // if (value) { try { entries_.insert(key, *value); } catch (...) {} }
      if (f->value) {
        try {
          f->self->entries_.insert(*f->key, *f->value);
        } catch (...) {
        }
      }
      // co_return value;
      ::new (async_destination_of(k, VALUE)(k))
          std::optional<std::string>(std::move(f->value));
      done = async_complete(k, VALUE);
      break;
    case ERROR:  // catch (std::error_code ec) { co_return with_error(ec); }
      ::new (async_destination_of(k, ERROR)(k)) std::error_code(f->ec);
      done = async_complete(k, ERROR);
      break;
    default:  // stopped: no handler sees it
      done = async_complete(k, STOPPED);
  }
  f->~cache_get_frame();  // and the frame is popped: our caller's top never moved
  return done;
}
// [[/tony]]

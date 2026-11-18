// What both designs share: the result slot and the cache's storage, as in
// kv-handle/common.hpp but without stdexec.
#pragma once

#include "async_abi.h"

#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

using str = std::string;
using value_t = std::optional<str>;

// Storage for any one declared completion's payload. Whoever is told how the
// operation completed knows which member is alive, and destroys it.
template <class T>
union slot {
  T value;
  std::exception_ptr error;
  slot() noexcept {}
  ~slot() {}
  void destroy(int how) noexcept {
    if (how == ASYNC_VALUE) value.~T();
    else if (how == ASYNC_ERROR) error.~exception_ptr();
  }
};

class cache_entries {
 public:
  explicit cache_entries(bool retain = true) : retain_(retain) {}
  value_t find(str const& key) const {
    std::lock_guard lock(mutex_);
    if (auto it = entries_.find(key); it != entries_.end()) return it->second;
    return std::nullopt;
  }
  void insert(str key, str value) {
    std::lock_guard lock(mutex_);
    // retain_ == false keeps every lookup a miss, for measuring the miss path
    if (retain_) entries_.insert_or_assign(std::move(key), std::move(value));
  }

 private:
  bool retain_;
  mutable std::mutex mutex_;
  std::map<str, str> entries_;
};

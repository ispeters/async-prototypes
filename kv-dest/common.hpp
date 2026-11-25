// What every part of the experiment shares: the instrumented string, the
// result slot, the cache's storage, the backing store's data and a queue of
// deferred work. No stdexec: every variant here is handle-aware end to end.
#pragma once

#include "async_abi.h"

#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// ---- instrumented string, to count copies and moves -------------------------
#ifdef COUNT_MOVES
struct str {
    std::string s;
    // counted separately for keys ("k...") and values (everything else)
    inline static long key_copies, key_moves, val_copies, val_moves;
    static bool        is_key(const str& o) { return !o.s.empty() && o.s[0] == 'k'; }
    str() = default;
    str(const char* p) : s(p) {}
    str(const str& o) : s(o.s) { ++(is_key(o) ? key_copies : val_copies); }
    str(str&& o) noexcept {
        ++(is_key(o) ? key_moves : val_moves);
        s = std::move(o.s);
    }
    str& operator=(const str& o) {
        ++(is_key(o) ? key_copies : val_copies);
        s = o.s;
        return *this;
    }
    str& operator=(str&& o) noexcept {
        ++(is_key(o) ? key_moves : val_moves);
        s = std::move(o.s);
        return *this;
    }
    std::size_t size() const noexcept { return s.size(); }
    friend bool operator<(const str& a, const str& b) { return a.s < b.s; }
    friend bool operator==(const str& a, const str& b) { return a.s == b.s; }
};
#else
using str = std::string;
#endif

using value_t = std::optional<str>;

// ---- the result slot --------------------------------------------------------
// Storage for any one declared completion's payload. Whoever is told how the
// operation completed knows which member is alive, and destroys it.
template <class T>
union slot {
    T                  value;
    std::exception_ptr error;
    slot() noexcept {}
    ~slot() {}
    void destroy(int how) noexcept {
        if (how == ASYNC_VALUE) {
            value.~T();
        } else if (how == ASYNC_ERROR) {
            error.~exception_ptr();
        }
    }
};

// ---- the cache's storage (as in kv-handle) ----------------------------------
class cache_entries {
  public:
    explicit cache_entries(bool retain = true) : retain_(retain) {}
    value_t find(const str& key) const {
        std::lock_guard lock(mutex_);
        if (auto it = entries_.find(key); it != entries_.end()) {
            return it->second;
        }
        return std::nullopt;
    }
    void insert(str key, str value) {
        std::lock_guard lock(mutex_);
        // retain_ == false keeps every lookup a miss, for measuring the miss path
        if (retain_) {
            entries_.insert_or_assign(std::move(key), std::move(value));
        }
    }

  private:
    bool               retain_;
    mutable std::mutex mutex_;
    std::map<str, str> entries_;
};

// ---- the backing store's data -----------------------------------------------
inline const std::exception_ptr g_not_found =
    std::make_exception_ptr(std::out_of_range("key absent"));

struct mem_data {
    std::map<str, str> data;
    bool               deferred;
    const str*         lookup(const str& key) const {
        if (auto it = data.find(key); it != data.end()) {
            return &it->second;
        }
        return nullptr;
    }
    value_t find(const str& key) const {
        if (auto* v = lookup(key)) {
            return *v;
        }
        return std::nullopt;
    }
};

// ---- deferred work, standing in for an event loop ---------------------------
// The loop is the bottom of the machine stack: a task runs whatever its
// completion hands back before returning. (kv-handle's C queue returned an
// `async_next`; the handle type differs between the two columns here, so the
// task runs it itself.)
struct task {
    void (*run)(task*) noexcept;
    task* next;
};
inline task*  g_q_head;
inline task** g_q_tail = &g_q_head;
inline void   defer(task* t) noexcept {
    t->next   = nullptr;
    *g_q_tail = t;
    g_q_tail  = &t->next;
}
inline int run_pending() noexcept {
    int n = 0;
    while (g_q_head) {
        task* t  = g_q_head;
        g_q_head = t->next;
        if (!g_q_head) {
            g_q_tail = &g_q_head;
        }
        t->run(t);
        ++n;
    }
    return n;
}

// What every part of the experiment shares: the instrumented string, the
// result slot, the cache's storage, and P4223's alias for today's `function`.
#pragma once

#include "async_abi.h"
#include <exec/function.hpp>
#include <exec/get_frame_allocator.hpp>

#include <stdexec/execution.hpp>

#include <cstdlib>
#include <exception>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <utility>

namespace ex = STDEXEC;

// ---- instrumented string, to count copies and moves -------------------------
#ifdef COUNT_MOVES
struct str {
    std::string s;
    // counted separately for keys ("k...") and values ("v...")
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
    friend bool operator<(const str& a, const str& b) { return a.s < b.s; }
    friend bool operator==(const str& a, const str& b) { return a.s == b.s; }
};
#else
using str = std::string;
#endif

using value_t = std::optional<str>;

// ---- P4223's paper-local alias (async_fn.hpp in the paper) -------------------
template <class Sig>
struct async_fn_for;

template <class R, class... Args>
struct async_fn_for<R(Args...) const&> {
    using type =
        exec::function<ex::sender_tag(Args...) const&,
                       ex::completion_signatures<ex::set_value_t(R),
                                                 ex::set_error_t(std::exception_ptr),
                                                 ex::set_stopped_t()>>;
};

template <class Sig>
using async_fn = async_fn_for<Sig>::type;

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

// ---- the cache's storage (cache_entries.hpp in the paper) -------------------
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

// ---- an allocator over a strand of the async stack --------------------------
// What a caller puts in its environment as P4223's frame allocator.
template <class T>
struct strand_allocator {
    using value_type = T;
    async_sp* top;
    explicit strand_allocator(async_sp* t) noexcept : top(t) {}
    template <class U>
    strand_allocator(const strand_allocator<U>& o) noexcept : top(o.top) {}
    T* allocate(std::size_t n) noexcept {
        return static_cast<T*>(async_push(top, n * sizeof(T)));
    }
    void deallocate(T* p, std::size_t) noexcept { async_pop(top, p); }
    template <class U>
    bool operator==(const strand_allocator<U>& o) const noexcept {
        return top == o.top;
    }
};

// ---- the cursor as a query --------------------------------------------------
// Where the caller's strand of the async stack stands, or null from a caller
// that has none. A forwarding query, so C++26 adaptors pass it along, and
// `function` can forward it by naming it in `queries<...>`. The default means
// every environment answers it, so such a `function` still connects to a
// receiver that has never heard of it.
struct get_async_stack_t
    : ex::__query<get_async_stack_t, static_cast<async_sp*>(nullptr)> {
    static consteval auto query(ex::forwarding_query_t) noexcept -> bool { return true; }
};
inline constexpr get_async_stack_t get_async_stack{};

// What a caller with a strand puts in its environment: the strand as P4223's
// frame allocator, for C++26 senders below, and as the cursor.
struct strand_env {
    async_sp*                   top;
    strand_allocator<std::byte> query(exec::get_frame_allocator_t) const noexcept {
        return strand_allocator<std::byte>(top);
    }
    async_sp* query(get_async_stack_t) const noexcept { return top; }
};

// ---- inline-completion handshake for code that starts a C++26 sender ---------
// (translation 3 of the hand translations). A sender whose `start` returns a
// handle doesn't need it: the handle says whether the operation completed.
struct start_guard {
    void* op;
    int*  how;
};
inline thread_local start_guard tl_starting{nullptr, nullptr};

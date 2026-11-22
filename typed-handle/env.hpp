// Environments and stop tokens, as small stand-ins for C++26's.
//
// get_env(r) is r.get_env() if r has one, else an empty environment.
// get_stop_token(e) is e's answer to the query, else never_stop_token, whose
// stop_possible() is a constant false, so code that asks can be compiled
// away for receivers that can't be stopped.
//
// inplace_stop_source is simplified: callbacks are kept in an intrusive list
// behind a spin lock and run under it, so a callback mustn't deregister
// another, and a callback being run can't be deregistered from another
// thread. Nothing here stops from another thread.
#pragma once

#include <atomic>
#include <utility>

namespace th {

struct empty_env {};

template <class R>
TH_INLINE auto get_env(R const& r) noexcept {
  if constexpr (requires { r.get_env(); }) {
    return r.get_env();
  } else {
    return empty_env{};
  }
}
template <class R>
using env_of_t = decltype(th::get_env(std::declval<R const&>()));

template <class Q, class V>
struct prop {
  V value;
  TH_INLINE V query(Q) const noexcept { return value; }
};

template <class E1, class E2>
struct env {  // the join of two environments, the first winning
  E1 first;
  E2 second;
  template <class Q>
    requires requires(E1 const& a, E2 const& b, Q q) { a.query(q); } ||
             requires(E2 const& b, Q q) { b.query(q); }
  TH_INLINE auto query(Q q) const noexcept {
    if constexpr (requires { first.query(q); }) {
      return first.query(q);
    } else {
      return second.query(q);
    }
  }
};
template <class E1, class E2>
env(E1, E2) -> env<E1, E2>;

struct never_stop_token {
  static constexpr bool stop_requested() noexcept { return false; }
  static constexpr bool stop_possible() noexcept { return false; }
};

class inplace_stop_source;

class inplace_stop_token {
 public:
  constexpr inplace_stop_token() noexcept = default;
  explicit inplace_stop_token(inplace_stop_source const* s) noexcept
      : source_(s) {}
  TH_INLINE bool stop_requested() const noexcept;
  bool stop_possible() const noexcept { return source_ != nullptr; }
  inplace_stop_source const* source() const noexcept { return source_; }

 private:
  inplace_stop_source const* source_ = nullptr;
};

struct callback_base {
  void (*run)(callback_base*) noexcept;
  callback_base* next = nullptr;
  callback_base** prev = nullptr;
};

class inplace_stop_source {
 public:
  inplace_stop_source() noexcept = default;
  inplace_stop_source(inplace_stop_source&&) = delete;

  bool stop_requested() const noexcept {
    return requested_.load(std::memory_order_acquire);
  }
  inplace_stop_token get_token() const noexcept {
    return inplace_stop_token(this);
  }

  bool request_stop() noexcept {
    lock();
    if (requested_.load(std::memory_order_relaxed)) {
      unlock();
      return false;
    }
    requested_.store(true, std::memory_order_release);
    while (callback_base* c = head_) {
      head_ = c->next;
      if (head_) head_->prev = &head_;
      c->prev = nullptr;  // run, and no longer listed
      c->run(c);
    }
    unlock();
    return true;
  }

  // Registration: false (and nothing listed) if stop was already requested.
  bool add(callback_base* c) const noexcept {
    lock();
    if (requested_.load(std::memory_order_relaxed)) {
      unlock();
      return false;
    }
    c->next = head_;
    c->prev = &head_;
    if (head_) head_->prev = &c->next;
    head_ = c;
    unlock();
    return true;
  }
  void remove(callback_base* c) const noexcept {
    lock();
    if (c->prev) {
      *c->prev = c->next;
      if (c->next) c->next->prev = c->prev;
    }
    unlock();
  }

 private:
  void lock() const noexcept {
    while (lock_.exchange(true, std::memory_order_acquire)) {
    }
  }
  void unlock() const noexcept { lock_.store(false, std::memory_order_release); }

  std::atomic<bool> requested_{false};
  mutable std::atomic<bool> lock_{false};
  mutable callback_base* head_ = nullptr;
};

TH_INLINE bool inplace_stop_token::stop_requested() const noexcept {
  return source_ && source_->stop_requested();
}

// Runs f when stop is requested on the token, or at once if it already has
// been. Deregisters when destroyed.
template <class F>
class inplace_stop_callback : callback_base {
 public:
  inplace_stop_callback(inplace_stop_token t, F f) noexcept
      : callback_base{&invoke}, f_(std::move(f)), source_(t.source()) {
    if (source_ && !source_->add(this)) {
      source_ = nullptr;
      f_();
    }
  }
  inplace_stop_callback(inplace_stop_callback&&) = delete;
  ~inplace_stop_callback() {
    if (source_) source_->remove(this);
  }

 private:
  static void invoke(callback_base* c) noexcept {
    static_cast<inplace_stop_callback*>(c)->f_();
  }
  F f_;
  inplace_stop_source const* source_;
};

struct get_stop_token_t {
  template <class E>
  TH_INLINE auto operator()(E const& e) const noexcept {
    if constexpr (requires { e.query(*this); }) {
      return e.query(*this);
    } else {
      return never_stop_token{};
    }
  }
};
inline constexpr get_stop_token_t get_stop_token{};

template <class R>
using stop_token_of_t = decltype(get_stop_token(get_env(std::declval<R const&>())));

template <class Token>
inline constexpr bool unstoppable = !Token::stop_possible();
template <>
inline constexpr bool unstoppable<inplace_stop_token> = false;

}  // namespace th

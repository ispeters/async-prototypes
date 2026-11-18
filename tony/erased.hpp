// The erased boundary under 4A (C++29 column of the erasure table).
#pragma once
#include "x29.hpp"
#include <optional>
#include <string>

namespace c29e {
using namespace x29;
using std::same_as;

// [[tony:erased]]
using value_t = std::optional<std::string>;

// the callee's side: what an implementation is connected to
class erased_receiver {
 public:
  using receiver_concept = receiver_t;

  explicit erased_receiver(continuation* k) noexcept : k_(k) {}

  template <class Sig>
  void* destination() noexcept {
    return table()->destination[index<Sig>](k_);
  }
  template <class Sig>
  operation_handle complete() noexcept {
    return {k_, index<Sig>};  // returned, not called
  }
  empty_env get_env() const noexcept { return {}; }  // not prototyped

  template <class Sig>
  static constexpr int index =
      same_as<Sig, set_value_t(value_t)>               ? 0
      : same_as<Sig, set_error_t(std::exception_ptr)> ? 1
                                                       : 2;

 private:
  using table_t = completion_table<2>;  // value, error
  table_t const* table() const noexcept {
    return static_cast<table_t const*>(k_->table);
  }
  continuation* k_;
};

// the caller's side: a composed receiver, erased
template <class Rcvr>
struct erased_record : continuation {
  Rcvr rcvr;

  explicit erased_record(Rcvr r)
      : continuation{&table_for}, rcvr(std::move(r)) {}

  static constexpr completion_table<2> table_for{
      [](continuation* k, int how) noexcept -> operation_handle {
        auto& r = static_cast<erased_record*>(k)->rcvr;
        switch (how) {
          case 0: return r.template complete<set_value_t(value_t)>();
          case 1: return r.template complete<
                      set_error_t(std::exception_ptr)>();
          default: return r.template complete<set_stopped_t()>();
        }
      },
      {[](continuation* k) noexcept -> void* {
         return static_cast<erased_record*>(k)
             ->rcvr.template destination<set_value_t(value_t)>();
       },
       [](continuation* k) noexcept -> void* {
         return static_cast<erased_record*>(k)
             ->rcvr.template destination<
                 set_error_t(std::exception_ptr)>();
       }}};
};
// [[/tony]]
}  // namespace c29e

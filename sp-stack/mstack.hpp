// The async stack as the machine stack, emulated on x86-64 System V with
// GCC or Clang and -fno-omit-frame-pointer.
//
// An asynchronous function's frame is its ordinary stack frame. The protocol
// is kv-handle's with the cursor removed: the stack pointer is the cursor.
//
//     ms::next entry(args..., slot* out, ms::cont* k);
//
// - Completing inline: the entry returns {k, how}, as any function returns.
// - Suspending: the entry returns {nullptr} (or another continuation that is
//   ready) *without popping its frame*. The caller gets control back with the
//   stack pointer below the callee's frame (MS_LEAVE).
// - Resuming: whoever runs a continuation first moves the stack pointer to
//   where that continuation's frame ends (cont::sp), so everything below it,
//   which is dead by then, is reused (ms_run).
// - A strand is a stack. Entering one, and running a continuation, switch
//   the stack pointer and nothing else (ms_enter, ms_run).
//
// What a compiler would do differently is listed in the README under
// "Emulation".
#pragma once

#include "async_abi.h"

#include <cstddef>
#include <vector>

#if !defined(__x86_64__) || !defined(__linux__)
#error "x86-64 Linux only"
#endif

namespace ms {

struct cont;
struct next {  // returned in rax:rdx
  cont* k;
  int how;
};

struct cont {
  next (*resume)(cont* self, int how) noexcept;
  void* sp;              // where the stack pointer stands when this continuation runs
  cont* link = nullptr;  // for the event queue
};

extern "C" {
// Calls fn(arg) with the stack pointer at `sp`; returns on the caller's stack
// whether fn returned normally or left frames behind.
next ms_enter(void* arg, void* sp, next (*fn)(void*) noexcept) noexcept;
// Runs k, then whatever each continuation hands back, each at its own `sp`.
void ms_run(cont* k, int how) noexcept;
}

// The stack pointer here. In a function that makes calls this is the low end
// of its frame.
#define MS_SP()                                \
  __extension__({                              \
    void* ms_sp_;                              \
    asm volatile("mov %%rsp, %0" : "=r"(ms_sp_)); \
    ms_sp_;                                    \
  })

// Returns `n` to this function's caller and leaves this function's frame, and
// everything below it, where it is. The push and `ret` pair with the caller's
// `call`, to keep the return predictor in step. The function must keep nothing
// in the red zone; one that makes a call (every user here does) doesn't.
#define MS_LEAVE(n)                            \
  do {                                         \
    ::ms::next ms_n_ = (n);                    \
    asm volatile("push 8(%%rbp)\n\t"           \
                 "mov (%%rbp), %%rbp\n\t"      \
                 "ret"                         \
                 :                             \
                 : "a"(ms_n_.k), "d"(ms_n_.how) \
                 : "memory");                  \
    __builtin_unreachable();                   \
  } while (0)

// After a call to an entry: a callee that left its frame didn't restore the
// callee-saved registers, so nothing may be kept in them across the call.
// (MS_NO_CLOBBER is for measuring what this costs; it's only safe on paths
// where nothing suspends.)
#ifndef MS_NO_CLOBBER
#define MS_CLOBBER() asm volatile("" : : : "rbx", "r12", "r13", "r14", "r15", "memory")
#else
#define MS_CLOBBER() ((void)0)
#endif

// Calls f() on the stack whose top is `sp`. f calls an entry, passing k as
// the continuation, and must be inlined here: mark the lambda always_inline.
template <class F>
struct on_strand {
  F* f;
  cont* k;
  static next call(void* v) noexcept {
    auto* self = static_cast<on_strand*>(v);
    next n = (*self->f)();
    MS_CLOBBER();
    if (n.k != self->k) MS_LEAVE(n);  // the callee left frames here
    return n;
  }
};
template <class F>
next enter(void* sp, cont* k, F&& f) noexcept {
  on_strand<F> s{&f, k};
  return ms_enter(&s, sp, &on_strand<F>::call);
}

inline void run(next n) noexcept {
  if (n.k) ms_run(n.k, n.how);
}

// The event queue: a continuation deferred here is run later, from the loop.
void defer(cont* k) noexcept;
int run_pending() noexcept;  // returns the number of continuations run

// Statistics and test hooks.
extern void* lowest_deferred_sp;        // lowest cont::sp passed to defer()
extern void (*after_pending)() noexcept; // called by the cache after its child suspends

// ---- where stacks come from --------------------------------------------------
struct stack {
  char* base = nullptr;
  std::size_t size = 0;
  void* top() const noexcept { return base + size; }
};

struct stack_provider {
  virtual stack acquire() noexcept = 0;
  virtual void release(stack) noexcept = 0;
  virtual ~stack_provider() = default;
};

// Pages from the operating system, with a guard page below each stack.
struct mmap_provider final : stack_provider {
  std::size_t size;
  bool pooled;
  std::vector<stack> free_;
  std::size_t mapped = 0;
  mmap_provider(std::size_t size, bool pooled) : size(size), pooled(pooled) {}
  ~mmap_provider();
  stack acquire() noexcept override;
  void release(stack s) noexcept override;
  static stack map(std::size_t size) noexcept;  // {} on failure
  static void unmap(stack s) noexcept;
};

// Fixed-size pieces of one buffer the user supplies. No guard pages, and no
// system call after construction.
struct arena_provider final : stack_provider {
  char* buf;
  std::size_t chunk, count, next_ = 0;
  std::vector<stack> free_;
  arena_provider(char* buf, std::size_t bytes, std::size_t chunk)
    : buf(buf), chunk(chunk), count(bytes / chunk) {}
  stack acquire() noexcept override;
  void release(stack s) noexcept override { free_.push_back(s); }
};

// How far down a stack was written, for stacks filled with `fill` beforehand.
void fill(stack s) noexcept;
std::size_t high_water(stack s) noexcept;

}  // namespace ms

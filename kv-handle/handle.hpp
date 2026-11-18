// The sender protocol with a `start` that returns a handle, as far as this
// experiment builds it, and `hfn`, a stand-in for P4223's `function` written
// against it.
#pragma once

#include "common.hpp"

#include <concepts>
#include <tuple>
#include <type_traits>

// ---- what a handle-aware receiver offers ------------------------------------
// A result slot, a continuation record and a cursor into the async stack. A
// real one would also be a C++26 receiver; here it's only the three pointers,
// and it's recognized by its type.
template <class T>
struct handle_rcvr {
  slot<T>* out;
  async_cont* k;
  async_sp sp;
};

// A sender is handle-aware if connecting it to a handle-aware receiver gives
// an operation whose `start` returns a handle. Such an operation is a record
// of pointers: it needn't outlive the call to `start`, and whatever the
// operation keeps, `start` pushes on the async stack.
//
//   handle_sender<S, T>   an rvalue can be connected: the operation keeps
//                         nothing of the sender
//   handle_sender<S&, T>  an lvalue can be connected: the sender has to
//                         outlive the operation
template <class S, class T>
concept handle_sender = requires(S&& s, handle_rcvr<T> r) {
  { std::forward<S>(s).connect(r).start() } noexcept -> std::same_as<async_next>;
};

// ---- starting a C++26 sender from handle-aware code -------------------------
// What the lowering of `co_await sndr` does when `sndr` isn't handle-aware:
// connect it on the async stack, start it, and translate its completion. An
// inline completion comes back as the caller's own record, like any other. A
// later one has no way to pass a handle out through `set_value`, so the
// continuation runs nested inside it, as it does today.
namespace thunk {
  template <class T>
  struct frame_base {
    slot<T>* out;
    async_cont* k;
    async_sp top;  // this frame's strand, which the sender's own frames go on
    void (*destroy_op)(frame_base*) noexcept;

    void finish(int how) noexcept {
      if (tl_starting.op == this) {  // completed inside our call to start()
        *tl_starting.how = how;
        return;
      }
      async_cont* up = k;
      destroy_op(this);
      async_sp t;
      async_pop(&t, this);
      async_run({up, how});
    }
  };

  template <class T>
  struct rcvr {
    using receiver_concept = ex::receiver_tag;
    frame_base<T>* f;
    void set_value(T&& v) && noexcept {
      ::new (&f->out->value) T(std::move(v));
      f->finish(ASYNC_VALUE);
    }
    void set_error(std::exception_ptr e) && noexcept {
      ::new (&f->out->error) std::exception_ptr(std::move(e));
      f->finish(ASYNC_ERROR);
    }
    void set_stopped() && noexcept { f->finish(ASYNC_STOPPED); }
    // The async stack reaches what's below as P4223's frame allocator and as
    // the cursor.
    strand_env get_env() const noexcept { return {&f->top}; }
  };

  template <class T, class Sender>
  async_next start(Sender&& snd, slot<T>* out, async_cont* k, async_sp sp) noexcept {
    using op_t = ex::connect_result_t<Sender, rcvr<T>>;
    struct frame : frame_base<T> {
      union { op_t op; };
      frame() noexcept {}
      ~frame() {}
    };
    async_sp top = sp;
    auto* f = ::new (async_push(&top, sizeof(frame))) frame;
    f->out = out;
    f->k = k;
    f->top = top;
    f->destroy_op = [](frame_base<T>* b) noexcept { static_cast<frame*>(b)->op.~op_t(); };
    try {
      ::new (&f->op) op_t(ex::connect(std::forward<Sender>(snd), rcvr<T>{f}));
    } catch (...) {  // a throwing connect is an error completion
      async_pop(&top, f);
      ::new (&out->error) std::exception_ptr(std::current_exception());
      return {k, ASYNC_ERROR};
    }
    int how = ASYNC_PENDING;
    auto saved = std::exchange(tl_starting, start_guard{f, &how});
    ex::start(f->op);
    tl_starting = saved;
    if (how == ASYNC_PENDING) return {nullptr, ASYNC_PENDING};
    f->op.~op_t();
    async_pop(&top, f);
    return {k, how};
  }
}  // namespace thunk

// ---- connect, for a handle-aware receiver -----------------------------------
// Any sender connects to a handle-aware receiver: a handle-aware one directly,
// a C++26 one through the thunk. Either way the result is a record that needs
// to live only for the call to `start`.
template <class T, class S>
struct thunk_op {
  S&& snd;
  handle_rcvr<T> r;
  async_next start() noexcept { return thunk::start<T>(std::forward<S>(snd), r.out, r.k, r.sp); }
};

template <class T, class S>
auto hconnect(S&& snd, handle_rcvr<T> r) noexcept {
  if constexpr (handle_sender<S, T>) return std::forward<S>(snd).connect(r);
  else return thunk_op<T, S>{std::forward<S>(snd), r};
}

// ---- connect, for a C++26 receiver: the operation-state adaptor --------------
// A C++26 operation state around any handle-aware sender that names its
// `value_type`. It keeps the sender, and supplies what a handle-aware receiver
// would have offered: a slot, a continuation record that calls the receiver's
// completion, and a cursor. The cursor is the receiver's, if its environment
// has one for `get_async_stack` or its frame allocator is a strand; otherwise
// the operation roots a stack of its own.
template <class S, class R>
struct legacy_op : async_cont {
  using operation_state_concept = ex::operation_state_tag;
  using T = typename S::value_type;
  R rcvr;
  S snd;
  slot<T> out;
  async_sp root = nullptr;
  int how = ASYNC_PENDING;

  legacy_op(R r, S&& s) : rcvr(std::move(r)), snd(std::move(s)) {
    resume = [](async_cont* c, int h) noexcept -> async_next {
      static_cast<legacy_op*>(c)->complete(h);
      return {nullptr, ASYNC_PENDING};
    };
  }
  legacy_op(legacy_op&&) = delete;
  ~legacy_op() {
    out.destroy(how);
    if (root) async_stack_destroy(root);
  }

  async_sp cursor() noexcept {
    if (async_sp* top = get_async_stack(ex::get_env(rcvr))) {
      return *top;
    } else if constexpr (requires {
                           requires std::same_as<
                               std::decay_t<decltype(exec::get_frame_allocator(ex::get_env(rcvr)))>,
                               strand_allocator<std::byte>>;
                         }) {
      return *exec::get_frame_allocator(ex::get_env(rcvr)).top;
    } else {
      return root = async_stack_create();
    }
  }

  void start() & noexcept {
    async_next n = snd.connect(handle_rcvr<T>{&out, this, cursor()}).start();
    if (n.next == this) complete(n.how);
    else async_run(n);  // this operation is the bottom of the stack
  }
  void complete(int h) noexcept {
    how = h;
    if (h == ASYNC_VALUE) ex::set_value(std::move(rcvr), std::move(out.value));
    else if (h == ASYNC_ERROR) ex::set_error(std::move(rcvr), std::move(out.error));
    else ex::set_stopped(std::move(rcvr));
  }
};

// A C++26 sender around a handle-aware one, for senders that don't provide
// the C++26 `connect` themselves.
template <class S>
struct cxx26_sender {
  using sender_concept = ex::sender_tag;
  using completion_signatures = ex::completion_signatures<
      ex::set_value_t(typename S::value_type), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;
  S snd;
  template <ex::receiver R>
  legacy_op<S, R> connect(R r) && { return legacy_op<S, R>(std::move(r), std::move(snd)); }
};

template <class S>
cxx26_sender<std::decay_t<S>> as_cxx26(S&& snd) {
  return {std::forward<S>(snd)};
}

// ---- hfn: `function`, if `start` could return a handle ----------------------
// Declared and constructed as P4223's is:
//
//   hfn<value_t(str) const&> get(str key) const {
//     return {*this, std::move(key), [](store const& self, str key) { return <sender>; }};
//   }
//
// It holds the curried arguments and one erased entry, which the constructor
// generates from the factory. Which entry depends on the sender the factory
// returns:
//
//   handle-aware, keeps nothing  the factory took its arguments by reference:
//                                call it, connect, start. No frame.
//   handle-aware, owns something the sender gets a frame on the async stack,
//                                popped when the operation completes.
//   a C++26 sender               call it, connect through the thunk, start.
//
// Its `connect` is chosen by the receiver: a handle-aware receiver gets an
// operation whose `start` returns a handle; a C++26 receiver gets the
// operation-state adaptor.
template <class Sig>
class hfn;

template <class T, class... Args>
class hfn<T(Args...) const&> {
  using entry_t = async_next (*)(hfn& fn, slot<T>* out, async_cont* k, async_sp sp) noexcept;

  entry_t entry_;                // the erased `start`
  void const* self_;             // curried: the object
  std::tuple<Args...> args_;     // curried: the arguments

  template <class Self, class Factory>
  struct call {
    // A factory that can't take rvalues borrows the curried arguments.
    static constexpr bool borrows = !std::invocable<Factory, Self const&, Args...>;
    static decltype(auto) make(Self const& self, Args&... args) {
      if constexpr (borrows) return Factory()(self, args...);
      else return Factory()(self, std::move(args)...);
    }
    using sender_t = decltype(make(std::declval<Self const&>(), std::declval<Args&>()...));
  };

  static async_next fail(slot<T>* out, async_cont* k) noexcept {
    ::new (&out->error) std::exception_ptr(std::current_exception());
    return {k, ASYNC_ERROR};
  }

  template <class Self, class Factory>
  static async_next entry_for(hfn& fn, slot<T>* out, async_cont* k, async_sp sp) noexcept {
    using S = typename call<Self, Factory>::sender_t;
    Self const& self = *static_cast<Self const*>(fn.self_);
    return std::apply(
        [&](Args&... args) noexcept -> async_next {
          if constexpr (handle_sender<S&, T> && !handle_sender<S, T>) {
            struct frame : async_cont {
              async_cont* k;
              union { S snd; };
              frame() noexcept {}
              ~frame() {}
              async_next done(int how) noexcept {
                snd.~S();
                async_cont* up = k;
                async_sp t;
                async_pop(&t, this);
                return {up, how};
              }
            };
            async_sp top = sp;
            auto* f = ::new (async_push(&top, sizeof(frame))) frame;
            f->k = k;
            f->resume = [](async_cont* c, int how) noexcept { return static_cast<frame*>(c)->done(how); };
            try {
              ::new (&f->snd) S(call<Self, Factory>::make(self, args...));
            } catch (...) {
              async_pop(&top, f);
              return fail(out, k);
            }
            async_next n = f->snd.connect(handle_rcvr<T>{out, f, top}).start();
            if (n.next == f) return f->done(n.how);
            return n;
          } else {
            try {
              return hconnect(call<Self, Factory>::make(self, args...), handle_rcvr<T>{out, k, sp}).start();
            } catch (...) {  // a throwing factory is an error completion
              return fail(out, k);
            }
          }
        },
        fn.args_);
  }

 public:
  using sender_concept = ex::sender_tag;
  using completion_signatures = ex::completion_signatures<
      ex::set_value_t(T), ex::set_error_t(std::exception_ptr), ex::set_stopped_t()>;
  using value_type = T;

  template <class Self, class Factory>
    requires std::is_empty_v<Factory> && std::default_initializable<Factory>
  hfn(Self const& self, Args&&... args, Factory)
    : entry_(&entry_for<Self, Factory>), self_(&self), args_(std::move(args)...) {}

  // -- connected to a handle-aware receiver ---------------------------------
  struct op {
    hfn* fn;  // the sender outlives its operation, so its arguments are used in place
    handle_rcvr<T> r;
    async_next start() noexcept { return fn->entry_(*fn, r.out, r.k, r.sp); }
  };
  op connect(handle_rcvr<T> r) & noexcept { return {this, r}; }

  // -- connected to a C++26 receiver ----------------------------------------
  template <ex::receiver R>
  legacy_op<hfn, R> connect(R r) && { return legacy_op<hfn, R>(std::move(r), std::move(*this)); }
};

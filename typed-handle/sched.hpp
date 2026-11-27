// Asynchronous completion: a queue of erased handles, a run_loop and a
// thread pool that drain one, schedulers, schedule(), continues_on(),
// starts_on(), on(), and sync_wait().
//
// A completion that happens on another thread, or later, arrives through a
// queue, and what a queue holds is an erased handle: whoever drains it runs
// each handle with run(), so a typed chain still runs inline from there.
//
// schedule()'s completion signatures depend on the environment it's
// connected in: it declares set_stopped_t() only if the receiver's stop
// token can be stopped, and completes stopped if stop has been requested by
// the time it runs.
#pragma once
#include "algos.hpp"

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace th {

// ---- The queue ------------------------------------------------------------
// An item: what to run, and the queue's link. An operation state that waits
// in a queue derives from it.
struct task {
    task*         next = nullptr;
    erased_handle work;
};

// A FIFO of tasks, safe to push to and pop from on any thread. pop() waits
// for a task, and returns nullptr once finish() has been called and the
// queue is empty.
//
// push() and finish() notify while holding the lock. Either can be the last
// thing another thread does before the queue's owner stops waiting and
// destroys the queue (sync_wait's run_loop, when the completion arrives on a
// pool's thread), so the notifier must be done with the condition variable
// before the waiter can return.
class task_queue {
  public:
    void push(task* t) noexcept {
        std::lock_guard lock(mutex_);
        t->next = nullptr;
        *tail_  = t;
        tail_   = &t->next;
        cv_.notify_one();
    }

    task* pop() noexcept {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return head_ != nullptr || finishing_; });
        task* t = head_;
        if (t != nullptr) {
            head_ = t->next;
            if (head_ == nullptr) {
                tail_ = &head_;
            }
        }
        return t;
    }

    void finish() noexcept {
        std::lock_guard lock(mutex_);
        finishing_ = true;
        cv_.notify_all();
    }

    // Run tasks until finish() has been called and none are left. A task's
    // handle is copied out first: running it may end the task's lifetime.
    void drain() noexcept {
        while (task* t = pop()) {
            erased_handle work = t->work;
            th::run(work);
        }
    }

  private:
    std::mutex              mutex_;
    std::condition_variable cv_;
    task*                   head_      = nullptr;
    task**                  tail_      = &head_;
    bool                    finishing_ = false;
};

// ---- schedule() -----------------------------------------------------------
struct schedule_sender {
    task_queue* queue;

    template <class Env>
    static consteval auto get_completion_signatures() {
        using stopped = std::conditional_t<
            unstoppable<decltype(get_stop_token(std::declval<const Env&>()))>,
            completion_signatures<>,
            completion_signatures<set_stopped_t()>>;
        return concat_t<completion_signatures<set_value_t()>, stopped>();
    }

    template <class R>
    struct op : task {
        R           rcvr;
        task_queue* queue;

        op(R r, task_queue* q) noexcept : rcvr(std::move(r)), queue(q) {}
        op(op&&) = delete;

        // Run from the queue: complete, then settle what the completion hands
        // back, so that a typed chain runs inline here.
        static erased_handle execute(void* p) noexcept {
            auto& self = *static_cast<op*>(p);
            if constexpr (unstoppable<stop_token_of_t<R>>) {
                return settle(th::set_value(self.rcvr));
            } else {
                return settle(either(
                    get_stop_token(th::get_env(self.rcvr)).stop_requested(),
                    [&] { return th::set_stopped(self.rcvr); },
                    [&] { return th::set_value(self.rcvr); }));
            }
        }

        TH_INLINE no_operation start() & noexcept {
            this->work = erased_handle(&execute, static_cast<void*>(this));
            queue->push(this);
            return {};
        }
    };

    template <class R>
    op<R> connect(R rcvr) && noexcept {
        return op<R>(std::move(rcvr), queue);
    }
};

// A scheduler: something to schedule() on, and a queue that something
// drains.
struct queue_scheduler {
    task_queue*     queue;
    schedule_sender schedule() const noexcept { return {queue}; }
    friend bool     operator==(queue_scheduler, queue_scheduler) = default;
};

template <class Sch>
TH_INLINE auto schedule(Sch sch) noexcept {
    return sch.schedule();
}

// ---- run_loop and thread_pool ---------------------------------------------
// A run_loop is drained by whoever calls run(), until finish().
class run_loop {
  public:
    queue_scheduler get_scheduler() noexcept { return {&queue_}; }
    void            run() noexcept { queue_.drain(); }
    void            finish() noexcept { queue_.finish(); }

  private:
    task_queue queue_;
};

// A thread_pool is drained by its own threads, until it is destroyed.
class thread_pool {
  public:
    explicit thread_pool(std::size_t n) {
        threads_.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            threads_.emplace_back([this] { queue_.drain(); });
        }
    }
    thread_pool(thread_pool&&) = delete;
    ~thread_pool() {
        queue_.finish();
        for (auto& t : threads_) {
            t.join();
        }
    }
    queue_scheduler get_scheduler() noexcept { return {&queue_}; }

  private:
    task_queue               queue_;
    std::vector<std::thread> threads_;
};

// ---- continues_on ---------------------------------------------------------
// Completes as the child does, but from sch's context: when the child
// completes, schedule on sch, then deliver. The child's payload goes straight
// to the receiver's destination where its signature is already decayed, so
// nothing is moved for the hop; a signature that decays (a reference) is
// staged here and decay-copied after the hop, as C++26 does.
template <class Sig>
inline constexpr bool decays = !std::same_as<Sig, decayed_t<Sig>>;

// Whether decay-copying a completion's arguments can't throw.
template <class Sig>
inline constexpr bool nothrow_decay_copy = false;
template <class Tag, class... As>
inline constexpr bool nothrow_decay_copy<Tag(As...)> =
    nothrow_payload<Tag(std::decay_t<As>...), As...>;

template <class Child, class Sch, class R>
struct continues_on_op {
    using env_t                    = env_of_t<R>;
    using child_list               = as_list_t<completions_of_t<Child, env_t>>;
    using hop_sender               = decltype(th::schedule(std::declval<Sch>()));
    static constexpr std::size_t n = size_of<child_list>;

    struct child_rcvr : operation_handle<child_rcvr, continues_on_op> {
        using base_t = operation_handle<child_rcvr, continues_on_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            if constexpr (decays<Sig>) {
                return this->op()->staged.address();
            } else {
                return destination_of<R, Sig>(this->op()->rcvr);
            }
        }
        TH_INLINE env_t get_env() const noexcept { return th::get_env(this->op()->rcvr); }

      private:
        friend base_t;

        // Remember which completion arrived, then hop.
        template <class Sig>
        TH_INLINE static auto resume(continues_on_op& self) noexcept {
            self.which = int(index_in<Sig, child_list>);
            return start_later(self.hop);
        }
    };

    struct hop_rcvr : operation_handle<hop_rcvr, continues_on_op> {
        using base_t = operation_handle<hop_rcvr, continues_on_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            static_assert(!has_payload<Sig>,
                          "a scheduler's completion carries nothing here");
            return nullptr;
        }
        TH_INLINE env_t get_env() const noexcept { return th::get_env(this->op()->rcvr); }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(continues_on_op& self) noexcept {
            if constexpr (std::same_as<Sig, set_value_t()>) {
                return self.deliver();
            } else {
                self.drop(); // the child's payload won't be delivered
                return later<Sig>(self.rcvr);
            }
        }
    };

    R                                      rcvr;
    int                                    which = -1;
    storage_for_t<child_list>              staged; // only for signatures that decay
    connect_result_t<Child, child_rcvr>    child;
    connect_result_t<hop_sender, hop_rcvr> hop;

    continues_on_op(Child&& c, Sch sch, R r)
        : rcvr(std::move(r)),
          child(th::connect(std::move(c), child_rcvr(this))),
          hop(th::connect(th::schedule(sch), hop_rcvr(this))) {}
    continues_on_op(continues_on_op&&) = delete;

    TH_INLINE auto start() & noexcept { return th::start(child); }

    TH_INLINE auto deliver() noexcept {
        return by_index<n>(which, [this]<std::size_t I>() {
            using Sig = nth_t<I, child_list>;
            if constexpr (!decays<Sig>) {
                return later<Sig>(rcvr);
            } else {
                return apply_payload<Sig>(staged.address(), [this](auto&&... as) {
                    return try_complete_with<decayed_t<Sig>>(
                        rcvr, static_cast<decltype(as)&&>(as)...);
                });
            }
        });
    }

    void drop() noexcept {
        for_each_index<n>([&]<std::size_t I>() {
            using Sig = nth_t<I, child_list>;
            if (which == int(I)) {
                if constexpr (decays<Sig>) {
                    destroy_payload<Sig>(staged.address());
                } else if constexpr (has_payload<Sig>) {
                    destroy_payload<Sig>(destination_of<R, Sig>(rcvr));
                }
            }
        });
    }
};

// The scheduler's completions other than its value.
template <class Sch, class Env>
using hop_extra_sigs = to_sigs_t<
    decltype(select_t<
                 set_error_t,
                 completions_of_t<decltype(th::schedule(std::declval<Sch>())), Env>>() +
             select_t<
                 set_stopped_t,
                 completions_of_t<decltype(th::schedule(std::declval<Sch>())), Env>>())>;

template <class Child, class Sch>
struct continues_on_sender {
    Child child;
    Sch   sch;

    template <class Env>
    static consteval auto get_completion_signatures() {
        using child_list              = as_list_t<completions_of_t<Child, Env>>;
        constexpr bool nothrow_copies = []<class... S>(type_list<S...>) {
            return ((!decays<S> || nothrow_decay_copy<S>) && ...);
        }(child_list());
        using decayed = decltype([]<class... S>(type_list<S...>) {
            return concat_t<completion_signatures<>,
                            completion_signatures<decayed_t<S>>...>();
        }(child_list()));
        return concat_t<decayed, hop_extra_sigs<Sch, Env>, eptr_unless<nothrow_copies>>();
    }

    template <class R>
    continues_on_op<Child, Sch, R> connect(R rcvr) && {
        return {std::move(child), sch, std::move(rcvr)};
    }
};
template <class Child, class Sch>
continues_on_sender<Child, Sch> continues_on(Child c, Sch sch) {
    return {std::move(c), sch};
}

// ---- starts_on ------------------------------------------------------------
// Schedules on sch, then starts the child there; the child's environment
// says its scheduler is sch.
template <class Sch, class Env>
using scheduler_env = env<prop<get_scheduler_t, Sch>, Env>;

template <class Sch, class Child, class R>
struct starts_on_op {
    using env_t      = scheduler_env<Sch, env_of_t<R>>;
    using hop_sender = decltype(th::schedule(std::declval<Sch>()));

    struct child_rcvr : operation_handle<child_rcvr, starts_on_op> {
        using base_t = operation_handle<child_rcvr, starts_on_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            return destination_of<R, Sig>(this->op()->rcvr);
        }
        TH_INLINE env_t get_env() const noexcept {
            return {{this->op()->sch}, th::get_env(this->op()->rcvr)};
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(starts_on_op& self) noexcept {
            return later<Sig>(self.rcvr);
        }
    };

    struct hop_rcvr : operation_handle<hop_rcvr, starts_on_op> {
        using base_t = operation_handle<hop_rcvr, starts_on_op>;
        using base_t::base_t;

        template <class Sig>
        void* destination() noexcept {
            static_assert(!has_payload<Sig>,
                          "a scheduler's completion carries nothing here");
            return nullptr;
        }
        TH_INLINE env_of_t<R> get_env() const noexcept {
            return th::get_env(this->op()->rcvr);
        }

      private:
        friend base_t;

        template <class Sig>
        TH_INLINE static auto resume(starts_on_op& self) noexcept {
            if constexpr (std::same_as<Sig, set_value_t()>) {
                return start_later(self.child);
            } else {
                return later<Sig>(self.rcvr);
            }
        }
    };

    R                                      rcvr;
    Sch                                    sch;
    connect_result_t<Child, child_rcvr>    child;
    connect_result_t<hop_sender, hop_rcvr> hop;

    starts_on_op(Sch s, Child&& c, R r)
        : rcvr(std::move(r)),
          sch(s),
          child(th::connect(std::move(c), child_rcvr(this))),
          hop(th::connect(th::schedule(s), hop_rcvr(this))) {}
    starts_on_op(starts_on_op&&) = delete;

    TH_INLINE auto start() & noexcept { return th::start(hop); }
};

template <class Sch, class Child>
struct starts_on_sender {
    Sch   sch;
    Child child;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return concat_t<completions_of_t<Child, scheduler_env<Sch, Env>>,
                        hop_extra_sigs<Sch, Env>>();
    }

    template <class R>
    starts_on_op<Sch, Child, R> connect(R rcvr) && {
        return {sch, std::move(child), std::move(rcvr)};
    }
};
template <class Sch, class Child>
starts_on_sender<Sch, Child> starts_on(Sch sch, Child c) {
    return {sch, std::move(c)};
}

// ---- on -------------------------------------------------------------------
// Starts the child on sch, then comes back to the scheduler of the
// receiver's environment: starts_on, then continues_on. Its completions
// depend on that scheduler's type, so on an environment without one it has
// none to offer, and connecting fails.
template <class Env>
using scheduler_of_t = decltype(get_scheduler(std::declval<const Env&>()));

template <class Sch, class Child>
struct on_sender {
    Sch   sch;
    Child child;

    template <class Env>
    static consteval auto get_completion_signatures() {
        return completions_of_t<
            continues_on_sender<starts_on_sender<Sch, Child>, scheduler_of_t<Env>>,
            Env>();
    }

    template <class R>
    auto connect(R rcvr) && {
        auto back = get_scheduler(th::get_env(rcvr));
        return th::connect(continues_on(starts_on(sch, std::move(child)), back),
                           std::move(rcvr));
    }
};
template <class Sch, class Child>
on_sender<Sch, Child> on(Sch sch, Child c) {
    return {sch, std::move(c)};
}

// ---- sync_wait() ----------------------------------------------------------
// Connects to a receiver whose environment's scheduler is a run_loop on the
// calling thread, starts, and drains that loop until the operation
// completes. Returns the values as a tuple, or nothing if stopped; rethrows
// an exception_ptr and throws any other error.
using sync_wait_env = prop<get_scheduler_t, queue_scheduler>;

template <class Sigs>
struct sync_wait_state {
    using list = as_list_t<Sigs>;
    storage_for_t<list> storage;
    int                 which = -1;
    run_loop            loop;

    sync_wait_state()                  = default;
    sync_wait_state(sync_wait_state&&) = delete;
    ~sync_wait_state() {
        for_each_index<size_of<list>>([&]<std::size_t I>() {
            if (which == int(I)) {
                destroy_payload<nth_t<I, list>>(storage.address());
            }
        });
    }
};

template <class Sigs>
struct sync_wait_rcvr : operation_handle<sync_wait_rcvr<Sigs>, sync_wait_state<Sigs>> {
    using state_t = sync_wait_state<Sigs>;
    using base_t  = operation_handle<sync_wait_rcvr, state_t>;
    using base_t::base_t;

    TH_INLINE sync_wait_env get_env() const noexcept {
        return {this->op()->loop.get_scheduler()};
    }
    template <class Sig>
    void* destination() noexcept {
        return this->op()->storage.address();
    }

  private:
    friend base_t;

    // Record which completion arrived, then let the loop's run() return.
    template <class Sig>
    TH_INLINE static no_operation resume(state_t& self) noexcept {
        self.which = int(index_in<Sig, typename state_t::list>);
        self.loop.finish();
        return {};
    }
};

template <class Sig>
struct decayed_tuple;
template <class Tag, class... As>
struct decayed_tuple<Tag(As...)> {
    using type = std::tuple<std::decay_t<As>...>;
};

template <class S>
auto sync_wait(S&& s) {
    using Sigs   = completions_of_t<S, sync_wait_env>;
    using list   = as_list_t<Sigs>;
    using values = select_t<set_value_t, Sigs>;
    static_assert(size_of<values> == 1, "sync_wait needs exactly one value completion");
    using value_sig = nth_t<0, values>;
    using result    = std::optional<typename decayed_tuple<value_sig>::type>;

    sync_wait_state<Sigs> st;
    auto                  op = th::connect(std::forward<S>(s), sync_wait_rcvr<Sigs>(&st));
    th::run(th::start(op));
    st.loop.run();

    if (st.which == int(index_in<value_sig, list>)) {
        return apply_payload<value_sig>(st.storage.address(), [](auto&&... as) {
            return result(std::in_place, static_cast<decltype(as)&&>(as)...);
        });
    }
    for_each_index<size_of<list>>([&]<std::size_t I>() {
        using Sig = nth_t<I, list>;
        if constexpr (std::same_as<tag_of_t<Sig>, set_error_t>) {
            if (st.which == int(I)) {
                apply_payload<Sig>(st.storage.address(), [](auto&& e) {
                    if constexpr (std::same_as<std::decay_t<decltype(e)>,
                                               std::exception_ptr>) {
                        std::rethrow_exception(e);
                    } else {
                        throw std::decay_t<decltype(e)>(static_cast<decltype(e)&&>(e));
                    }
                });
            }
        }
    });
    return result(); // stopped
}

} // namespace th

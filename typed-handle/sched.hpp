// Asynchronous completion: a queue of erased handles, a run_loop and a
// thread pool that drain one, schedulers, schedule(), and sync_wait().
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

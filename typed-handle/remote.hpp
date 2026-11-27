// Callees compiled in their own translation unit (remote.cpp), for the
// erased boundary: each declares what the caller needs and nothing else.
// remote.cpp checks the declared sizes. It must be built with the same
// TH_MODE as its caller.
#pragma once
#include "algos.hpp"

#include <cstddef>

namespace th {

// then(just(x), inc)
struct remote_inc {
    using sigs                         = completion_signatures<set_value_t(int)>;
    static constexpr std::size_t size  = 40;
    static constexpr std::size_t align = alignof(void*);
    static void                  connect(void* frame, record* k, int x) noexcept;
    static erased_handle         start(void* frame) noexcept;
    static void                  destroy(void* frame) noexcept;
};

// then(just_unless_stopped(x), inc), which asks the caller's stop token
struct remote_inc_checked {
    using sigs = completion_signatures<set_value_t(int), set_stopped_t()>;
    static constexpr std::size_t size  = 40;
    static constexpr std::size_t align = alignof(void*);
    static void                  connect(void* frame, record* k, int x) noexcept;
    static erased_handle         start(void* frame) noexcept;
    static void                  destroy(void* frame) noexcept;
};

} // namespace th

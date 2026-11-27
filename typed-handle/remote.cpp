// The callees declared in remote.hpp, compiled on their own: the caller
// sees only their entry points.
#include "remote.hpp"

#include <memory>
#include <new>

namespace th {
namespace {

constexpr auto inc = [](int v) noexcept { return v + 1; };

template <class Callee, class Child>
using op_t = connect_result_t<Child, erased_receiver<typename Callee::sigs>>;

template <class Callee, class Child>
constexpr bool fits = sizeof(op_t<Callee, Child>) <= Callee::size &&
                      alignof(op_t<Callee, Child>) <= Callee::align;

using inc_child     = decltype(then(just(0), inc));
using checked_child = decltype(then(just_unless_stopped(0), inc));

} // namespace

static_assert(fits<remote_inc, inc_child>);
static_assert(fits<remote_inc_checked, checked_child>);
static_assert(
    same_as<typename remote_inc::sigs, completions_of_t<inc_child, erased_env>>);
static_assert(same_as<typename remote_inc_checked::sigs,
                      completions_of_t<checked_child, erased_env>>);

void remote_inc::connect(void* frame, record* k, int x) noexcept {
    ::new (frame) op_t<remote_inc, inc_child>(
        th::connect(then(just(x), inc), erased_receiver<sigs>{k}));
}
erased_handle remote_inc::start(void* frame) noexcept {
    return settle(th::start(*static_cast<op_t<remote_inc, inc_child>*>(frame)));
}
void remote_inc::destroy(void* frame) noexcept {
    std::destroy_at(static_cast<op_t<remote_inc, inc_child>*>(frame));
}

void remote_inc_checked::connect(void* frame, record* k, int x) noexcept {
    ::new (frame) op_t<remote_inc_checked, checked_child>(
        th::connect(then(just_unless_stopped(x), inc), erased_receiver<sigs>{k}));
}
erased_handle remote_inc_checked::start(void* frame) noexcept {
    return settle(
        th::start(*static_cast<op_t<remote_inc_checked, checked_child>*>(frame)));
}
void remote_inc_checked::destroy(void* frame) noexcept {
    std::destroy_at(static_cast<op_t<remote_inc_checked, checked_child>*>(frame));
}

} // namespace th

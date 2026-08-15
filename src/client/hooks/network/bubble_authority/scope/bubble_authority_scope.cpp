#include "bubble_authority_scope.h"

#include <cstddef>

namespace sunrise::client::hooks::network::bubble_authority::scope {
namespace {

thread_local std::size_t g_depth{};

} // namespace

/** Enters one decoder-local authority scope on the current thread. */
void enter() noexcept {
    ++g_depth;
}

/** Leaves one decoder-local authority scope on the current thread. */
void leave() noexcept {
    --g_depth;
}

/** @return True while the current thread is inside an admitted decoder call. */
bool active() noexcept {
    return g_depth != 0;
}

} // namespace sunrise::client::hooks::network::bubble_authority::scope

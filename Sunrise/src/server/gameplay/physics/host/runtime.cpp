#include "runtime.h"

#include <memory>
#include <new>

#include "state/gameplay/physics/runtime.h"

namespace sunrise::server::gameplay::physics::host::runtime {
namespace {

std::unique_ptr<BubbleHost> g_host{};

} // namespace

/** Creates the process host and dormant entity-state registry. */
bool initialize() noexcept {
    if (g_host != nullptr) {
        return g_host->ready();
    }
    state::gameplay::physics::initialize();
    std::unique_ptr<BubbleHost> candidate{new (std::nothrow) BubbleHost{}};
    if (candidate == nullptr || !candidate->ready()) {
        state::gameplay::physics::shutdown();
        return false;
    }
    g_host = std::move(candidate);
    return true;
}

/** Returns the initialized process host. */
BubbleHost* instance() noexcept {
    return g_host.get();
}

/** Returns the immutable initialized process host. */
const BubbleHost* read_only_instance() noexcept {
    return g_host.get();
}

/** Releases host-owned worlds before clearing entity-state storage. */
void shutdown() noexcept {
    g_host.reset();
    state::gameplay::physics::shutdown();
}

} // namespace sunrise::server::gameplay::physics::host::runtime

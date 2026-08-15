#include <Windows.h>

#include <atomic>

#include "runtime.h"

namespace sunrise::state::activity {
namespace {

/**
 * How far the client has got through the current destination load.
 * The spawn gate writes and reads this, so it is an atomic, not lock-guarded State. It is one
 * observed value and no transaction depends on it.
 */
std::atomic<WorldPhase> g_phase{WorldPhase::idle};
/** Tick the current load started on. Read only while the phase is transitioning. */
std::atomic<std::uint64_t> g_transitionTick{};

} // namespace

/** Records how far the current destination load has got. */
void note_world_phase(WorldPhase phase) noexcept {
    const WorldPhase previous = g_phase.load(std::memory_order_relaxed);
    // Stamped before the phase, so a reader that sees transitioning never reads a stale tick.
    if (phase == WorldPhase::transitioning && previous != WorldPhase::transitioning) {
        g_transitionTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    g_phase.store(phase, std::memory_order_relaxed);
}

/** @return How far the client has got through the current destination load. */
WorldPhase world_phase() noexcept {
    return g_phase.load(std::memory_order_relaxed);
}

/** @return Milliseconds since the running load started, or zero when none is running. */
std::uint64_t world_transition_age() noexcept {
    if (g_phase.load(std::memory_order_relaxed) != WorldPhase::transitioning) {
        return 0;
    }
    const std::uint64_t started = g_transitionTick.load(std::memory_order_relaxed);
    const std::uint64_t now = GetTickCount64();
    return now > started ? now - started : 0;
}

} // namespace sunrise::state::activity

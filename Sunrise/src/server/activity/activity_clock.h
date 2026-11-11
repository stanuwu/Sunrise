#pragma once

#include <cstdint>
#include <limits>

#include "../../middleware/bap/activity_message/darkness_zone_auth.h"
#include "../../state/activity/runtime.h"

namespace sunrise::server::activity {

/**
 * Converts the host uptime elapsed since a millisecond origin to native activity-clock ticks.
 * @param nowMilliseconds Current host uptime in milliseconds.
 * @param originMilliseconds Host uptime the clock counts from.
 * @param output Receives native ticks, or zero on failure.
 * @return False for a future origin or a duration outside the native field width.
 */
[[nodiscard]] inline bool activity_clock_ticks(std::uint64_t nowMilliseconds,
                                               std::uint64_t originMilliseconds,
                                               std::uint64_t& output) noexcept {
    constexpr std::uint64_t kMillisecondsPerSecond = 1'000;
    constexpr std::uint64_t kTicksPerSecond =
        middleware::bap::activity_message::darkness_zone::kTicksPerSecond;
    output = 0;
    if (nowMilliseconds < originMilliseconds) {
        return false;
    }
    const std::uint64_t elapsedMilliseconds = nowMilliseconds - originMilliseconds;
    const std::uint64_t seconds = elapsedMilliseconds / kMillisecondsPerSecond;
    const std::uint64_t fraction =
        (elapsedMilliseconds % kMillisecondsPerSecond) * kTicksPerSecond / kMillisecondsPerSecond;
    if (seconds > ((std::numeric_limits<std::uint64_t>::max)() - fraction) / kTicksPerSecond) {
        return false;
    }
    output = seconds * kTicksPerSecond + fraction;
    return true;
}

/**
 * Converts the exact session generation's accepted-join origin to native clock ticks.
 * Public joins start at zero; private links keep their heartbeat clock unstarted.
 * @param sessionId Allocated activity session.
 * @param createdRevision Exact allocation generation.
 * @param nowMilliseconds Current host uptime in milliseconds.
 * @param start Whether this public join may start its clock.
 * @param output Receives ticks since origin, or zero for an unstarted or stale session.
 * @return False for a stale session, future origin or unrepresentable duration.
 */
[[nodiscard]] inline bool activity_clock_wire_ticks(std::uint64_t sessionId,
                                                    std::uint64_t createdRevision,
                                                    std::uint64_t nowMilliseconds,
                                                    bool start,
                                                    std::uint64_t& output) noexcept {
    output = 0;
    std::uint64_t origin = 0;
    bool started = false;
    if (!state::activity::native_clock_origin(
            sessionId, createdRevision, nowMilliseconds, start, origin, started)) {
        return false;
    }
    return !started || activity_clock_ticks(nowMilliseconds, origin, output);
}

} // namespace sunrise::server::activity

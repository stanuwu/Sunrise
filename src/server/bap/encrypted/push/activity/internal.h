#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../../../../middleware/bap/activity_message/sensor_auth_update.h"
#include "../../../internal.h"

namespace sunrise::server::bap::encrypted::push::activity {

namespace message = middleware::bap::activity_message::sensor_auth_update;

/**
 * Seeds the membership identity from the join when no identity message has arrived.
 * @param sessionId Joined activity session.
 * @param memberKey Client member key captured from the join request.
 * @param characterSoid Character the client signed in on.
 * @return True when a membership snapshot exists afterwards.
 */
[[nodiscard]] bool seed_identity(std::uint64_t sessionId,
                                 std::uint64_t memberKey,
                                 std::uint64_t characterSoid) noexcept;

/**
 * Seeds the transition token when nothing has published one.
 * @param sessionId Joined activity session.
 * @return True when the token is published.
 */
[[nodiscard]] bool seed_transition_token(std::uint64_t sessionId) noexcept;

/** Why one roster push produced nothing, or that it produced a body. */
enum class RosterOutcome : std::uint8_t {
    published,
    noEpoch,
    noLayout,
    noGroups,
    encodeFailed,
};

/**
 * Builds the roster body input for one session's current destination.
 * The state byte moves only while warming up or when the group set changes, because a changed
 * byte destructs and rebuilds every object the roster entries own. A burst send never moves it.
 * @param session Connection-owned epoch and roster counters, advanced on success.
 * @param scratch Lock-owned roster group storage the body's spans point into.
 * @param snapshot Cleared, then receives the epoch, roster and scalars.
 * @param destination Receives the destination name the push found.
 * @param destinationLength Receives its length.
 * @param burst True for a send on the loading cadence.
 * @return published when the destination gives a roster that binds the player, or why it did not.
 */
[[nodiscard]] RosterOutcome build_roster_snapshot(Session& session,
                                                  Scratch& scratch,
                                                  message::Snapshot& snapshot,
                                                  std::span<char> destination,
                                                  std::size_t& destinationLength,
                                                  bool burst) noexcept;

/**
 * Reports one roster push, and only when its outcome is new.
 * @param session Connection-owned roster counters, whose last reported reason is updated.
 * @param snapshot Body input, carrying the region, arrival and spawn set.
 * @param destination Destination name the push found; may be empty.
 * @param bytes Encoded body size, or zero when nothing was staged.
 * @param grant Bubble granted with this body, or -1.
 * @param outcome What the push produced.
 */
void report_roster_push(Session& session,
                        const message::Snapshot& snapshot,
                        std::string_view destination,
                        std::size_t bytes,
                        std::int32_t grant,
                        RosterOutcome outcome) noexcept;

} // namespace sunrise::server::bap::encrypted::push::activity

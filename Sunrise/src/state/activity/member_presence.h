#pragma once

#include "definition.h"
#include "membership/activity_membership_query.h"

namespace sunrise::state::activity::presence {
/** Reads only a joined native identity in the exact activity generation. */
[[nodiscard]] bool placement(const SessionBinding& binding,
                             std::uint64_t account,
                             std::uint64_t character,
                             membership::ClientPlacement& output) noexcept;
/** Reads this identity's latest committed join with a native region report. */
[[nodiscard]] bool latest_placement(std::uint64_t account,
                                    std::uint64_t character,
                                    membership::ClientPlacement& output) noexcept;
/**
 * Reads the activity session of this identity's latest committed native join.
 * A follower who joined without sending a launch request has no other source for the activity
 * he is in: his own character row still carries the value sign-on authored.
 * @return That session id, or zero when this identity is seated nowhere.
 */
[[nodiscard]] std::uint64_t seat_session(std::uint64_t account, std::uint64_t character) noexcept;
} // namespace sunrise::state::activity::presence

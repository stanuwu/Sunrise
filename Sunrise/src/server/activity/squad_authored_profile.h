#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "../../state/activity_sdk/format.h"

namespace sunrise::server::activity::activity_sdk_squads {

/** Selects the exact common profile of requested members without choosing an actor identity. */
[[nodiscard]] inline bool
select_authored_profile(std::span<const state::activity_sdk::format::SquadMember> members,
                        std::span<const std::int32_t> counts,
                        std::array<std::int8_t, 4>& output) noexcept {
    namespace format = state::activity_sdk::format;
    output = {};
    if (members.size() != counts.size()) {
        return false;
    }
    bool found = false;
    std::array<std::int8_t, 4> selected{};
    // A named type-2 member needs its parent's authored profile but zero loose actors, so an
    // all-zero request evaluates every member.
    bool loose = false;
    for (const std::int32_t count : counts) {
        if (count < 0) {
            return false;
        }
        loose = loose || count > 0;
    }
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (loose && counts[index] == 0) {
            continue;
        }
        const auto& member = members[index];
        if ((member.flags & format::kSquadMemberAuthoredProfileExact) == 0
            || !format::valid_authored_spawn_profile(member.authoredSpawnProfile)
            || (found && selected != member.authoredSpawnProfile)) {
            return false;
        }
        selected = member.authoredSpawnProfile;
        found = true;
    }
    if (found) {
        output = selected;
    }
    return found;
}

} // namespace sunrise::server::activity::activity_sdk_squads

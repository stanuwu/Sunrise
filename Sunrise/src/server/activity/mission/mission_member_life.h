#pragma once
#include "../../../state/activity/membership/mission_party.h"
#include "mission_script_player_sense.h"

namespace sunrise::server::activity::mission {
/** Only the owning connection's participation reports can establish this member's life. */
struct MissionMemberLife final {
    state::activity::membership::MissionMember member{};
    std::array<PlayerLifeObservation, kParticipationSlotCount> slots{};
    std::uint64_t sourceGeneration{};
    std::uint64_t playerKey{};

    /**
     * Binds this record to one committed member and its live connection.
     * A different member, join lifetime, connection generation or player key clears the retained
     * participation reports.
     * @param next Committed member and its join lifetime.
     * @param generation ActivityClient generation of the member's live connection, or 0 for none.
     * @param key Player key of that connection, or 0 for none.
     * @return True when the binding changed and the retained reports were cleared.
     */
    bool bind(const state::activity::membership::MissionMember& next,
              std::uint64_t generation,
              std::uint64_t key) noexcept {
        const bool changed = member.identity != next.identity
                             || member.joinedRevision != next.joinedRevision
                             || sourceGeneration != generation || playerKey != key;
        if (changed) {
            slots.fill({});
        }
        member = next;
        sourceGeneration = generation;
        playerKey = key;
        return changed;
    }

    /**
     * Reads this member's life from its own connection's participation reports.
     * @return The life in the one slot that carries this member's player key, or unknown when the
     * member has no live connection, or when no slot or several slots carry that key.
     */
    [[nodiscard]] PlayerLife life() const noexcept {
        if (!sourceGeneration || !playerKey) {
            return PlayerLife::unknown;
        }
        const PlayerLifeObservation* found = nullptr;
        for (const auto& slot : slots) {
            if (slot.playerKey != playerKey) {
                continue;
            }
            if (found) {
                return PlayerLife::unknown;
            }
            found = &slot;
        }
        return found ? found->life() : PlayerLife::unknown;
    }
};
} // namespace sunrise::server::activity::mission

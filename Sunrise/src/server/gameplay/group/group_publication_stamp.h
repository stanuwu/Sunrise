#pragma once

#include "../../../middleware/gameplay/group/session_state.h"

namespace sunrise::server::gameplay::group {
/** A successfully queued native snapshot, independent of its publication revision. */
struct PublicationStamp {
    std::uint32_t hash{};
    std::size_t members{};
    std::size_t players{};
    bool valid{};

    [[nodiscard]] static PublicationStamp
    from(const middleware::gameplay::group::MembershipUpdate& update) noexcept {
        auto content = update;
        content.revision = 0;
        return {middleware::gameplay::group::session_state_hash(content),
                content.members.size(),
                content.players.size(),
                true};
    }
    [[nodiscard]] bool matches(const PublicationStamp& other) const noexcept {
        return valid && other.valid && hash == other.hash && members == other.members
               && players == other.players;
    }
};
} // namespace sunrise::server::gameplay::group
